#include "TextureQuantizer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <unordered_map>

namespace cso_gui
{
	namespace
	{
		struct Color
		{
			std::uint8_t r, g, b;
		};

		struct Weighted
		{
			float c[3];
			float count;
		};

		inline std::uint32_t Pack(std::uint8_t r, std::uint8_t g, std::uint8_t b)
		{
			return (static_cast<std::uint32_t>(r) << 16) | (static_cast<std::uint32_t>(g) << 8) | b;
		}

		// Median cut over (colour, count) entries; returns up to `colors` boxes' means.
		std::vector<Weighted> MedianCut(std::vector<Weighted> entries, int colors)
		{
			struct Box
			{
				std::size_t begin, end;
			};
			std::vector<Box> boxes;
			boxes.push_back({ 0, entries.size() });

			const auto boxRange = [&entries](const Box &box, int &channel, float &spread)
			{
				float lo[3] = { 1e9f, 1e9f, 1e9f };
				float hi[3] = { -1e9f, -1e9f, -1e9f };
				for (std::size_t i = box.begin; i < box.end; ++i)
				{
					for (int k = 0; k < 3; ++k)
					{
						lo[k] = std::min(lo[k], entries[i].c[k]);
						hi[k] = std::max(hi[k], entries[i].c[k]);
					}
				}
				channel = 0;
				spread = hi[0] - lo[0];
				for (int k = 1; k < 3; ++k)
				{
					if (hi[k] - lo[k] > spread)
					{
						spread = hi[k] - lo[k];
						channel = k;
					}
				}
			};

			while (static_cast<int>(boxes.size()) < colors)
			{
				// Split the box with the largest spread * population.
				int bestBox = -1;
				double bestScore = 0.0;
				int bestChannel = 0;
				for (std::size_t b = 0; b < boxes.size(); ++b)
				{
					if (boxes[b].end - boxes[b].begin < 2)
						continue;
					int channel = 0;
					float spread = 0.0f;
					boxRange(boxes[b], channel, spread);
					if (spread <= 0.0f)
						continue;
					double population = 0.0;
					for (std::size_t i = boxes[b].begin; i < boxes[b].end; ++i)
						population += entries[i].count;
					const double score = static_cast<double>(spread) * std::sqrt(population);
					if (score > bestScore)
					{
						bestScore = score;
						bestBox = static_cast<int>(b);
						bestChannel = channel;
					}
				}
				if (bestBox < 0)
					break;

				Box box = boxes[static_cast<std::size_t>(bestBox)];
				std::sort(entries.begin() + static_cast<std::ptrdiff_t>(box.begin),
					entries.begin() + static_cast<std::ptrdiff_t>(box.end),
					[bestChannel](const Weighted &a, const Weighted &b)
					{
						// Total order (ties broken by the full colour): std::sort is
						// not stable, and equal keys would otherwise be arranged
						// differently by different standard libraries, giving a
						// different palette on MSVC and GCC for the same picture.
						if (a.c[bestChannel] != b.c[bestChannel])
							return a.c[bestChannel] < b.c[bestChannel];
						if (a.c[0] != b.c[0]) return a.c[0] < b.c[0];
						if (a.c[1] != b.c[1]) return a.c[1] < b.c[1];
						return a.c[2] < b.c[2];
					});
				double total = 0.0;
				for (std::size_t i = box.begin; i < box.end; ++i)
					total += entries[i].count;
				double running = 0.0;
				std::size_t split = box.begin + 1;
				for (std::size_t i = box.begin; i + 1 < box.end; ++i)
				{
					running += entries[i].count;
					split = i + 1;
					if (running >= total * 0.5)
						break;
				}
				boxes[static_cast<std::size_t>(bestBox)] = { box.begin, split };
				boxes.push_back({ split, box.end });
			}

			std::vector<Weighted> means;
			means.reserve(boxes.size());
			for (const Box &box : boxes)
			{
				double sum[3] = { 0, 0, 0 };
				double total = 0.0;
				for (std::size_t i = box.begin; i < box.end; ++i)
				{
					for (int k = 0; k < 3; ++k)
						sum[k] += static_cast<double>(entries[i].c[k]) * entries[i].count;
					total += entries[i].count;
				}
				Weighted mean{};
				for (int k = 0; k < 3; ++k)
					mean.c[k] = static_cast<float>(sum[k] / std::max(total, 1.0));
				mean.count = static_cast<float>(total);
				means.push_back(mean);
			}
			return means;
		}

		int Nearest(const std::vector<Color> &palette, int r, int g, int b)
		{
			int best = 0;
			int bestDistance = std::numeric_limits<int>::max();
			for (std::size_t i = 0; i < palette.size(); ++i)
			{
				const int dr = r - palette[i].r;
				const int dg = g - palette[i].g;
				const int db = b - palette[i].b;
				const int d = dr * dr + dg * dg + db * db;
				if (d < bestDistance)
				{
					bestDistance = d;
					best = static_cast<int>(i);
					if (d == 0)
						break;
				}
			}
			return best;
		}

		std::uint8_t ClampByte(float v)
		{
			return static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::lround(v)), 0, 255));
		}
	}

	IndexedTexture QuantizeRgba(const std::uint8_t *rgba, int width, int height,
		bool masked, int alphaThreshold, bool dither, int refineIterations)
	{
		if (!rgba || width <= 0 || height <= 0)
			throw std::runtime_error("quantizer: empty picture");
		const std::size_t pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);

		std::vector<std::uint8_t> opaque(pixels, 1);
		std::size_t transparentCount = 0;
		if (masked)
		{
			for (std::size_t i = 0; i < pixels; ++i)
			{
				opaque[i] = rgba[i * 4 + 3] >= alphaThreshold ? 1 : 0;
				transparentCount += opaque[i] ? 0 : 1;
			}
		}
		const int maxColors = masked ? 255 : 256;

		// Distinct opaque colours with their frequencies.
		std::unordered_map<std::uint32_t, std::uint32_t> histogram;
		for (std::size_t i = 0; i < pixels; ++i)
		{
			if (opaque[i])
				++histogram[Pack(rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2])];
		}
		std::vector<Weighted> distinct;
		distinct.reserve(histogram.size());
		for (const auto &[key, count] : histogram)
			distinct.push_back({ { static_cast<float>((key >> 16) & 255),
				static_cast<float>((key >> 8) & 255), static_cast<float>(key & 255) },
				static_cast<float>(count) });
		// unordered_map order is unspecified: sort so results are reproducible.
		std::sort(distinct.begin(), distinct.end(), [](const Weighted &a, const Weighted &b)
			{
				if (a.c[0] != b.c[0]) return a.c[0] < b.c[0];
				if (a.c[1] != b.c[1]) return a.c[1] < b.c[1];
				return a.c[2] < b.c[2];
			});

		std::vector<Color> palette;
		if (distinct.empty())
		{
			palette.push_back({ 0, 0, 0 });
		}
		else
		{
			std::vector<Weighted> means = MedianCut(distinct, maxColors);
			if (refineIterations > 0 && means.size() > 1)
			{
				std::vector<int> assign(distinct.size(), 0);
				for (int iteration = 0; iteration < refineIterations; ++iteration)
				{
					for (std::size_t i = 0; i < distinct.size(); ++i)
					{
						int best = 0;
						float bestDistance = std::numeric_limits<float>::max();
						for (std::size_t k = 0; k < means.size(); ++k)
						{
							const float dr = distinct[i].c[0] - means[k].c[0];
							const float dg = distinct[i].c[1] - means[k].c[1];
							const float db = distinct[i].c[2] - means[k].c[2];
							const float d = dr * dr + dg * dg + db * db;
							if (d < bestDistance)
							{
								bestDistance = d;
								best = static_cast<int>(k);
							}
						}
						assign[i] = best;
					}
					std::vector<double> sum(means.size() * 3, 0.0);
					std::vector<double> total(means.size(), 0.0);
					for (std::size_t i = 0; i < distinct.size(); ++i)
					{
						const std::size_t k = static_cast<std::size_t>(assign[i]);
						for (int c = 0; c < 3; ++c)
							sum[k * 3 + static_cast<std::size_t>(c)] +=
								static_cast<double>(distinct[i].c[c]) * distinct[i].count;
						total[k] += distinct[i].count;
					}
					for (std::size_t k = 0; k < means.size(); ++k)
					{
						if (total[k] <= 0.0)
							continue;  // an empty cluster keeps its colour
						for (int c = 0; c < 3; ++c)
							means[k].c[c] = static_cast<float>(sum[k * 3 + static_cast<std::size_t>(c)] / total[k]);
					}
				}
			}
			for (const Weighted &m : means)
				palette.push_back({ ClampByte(m.c[0]), ClampByte(m.c[1]), ClampByte(m.c[2]) });
		}

		const std::size_t used = palette.size();
		IndexedTexture result;
		result.width = width;
		result.height = height;
		result.indices.assign(pixels, 0);
		for (std::size_t i = 0; i < 256; ++i)
		{
			const Color &c = palette[std::min(i, used - 1)];  // padding repeats the last colour
			result.palette[i * 3] = c.r;
			result.palette[i * 3 + 1] = c.g;
			result.palette[i * 3 + 2] = c.b;
		}

		// Map pixels to the palette (only the `used` real entries are candidates).
		// Transparent texels contribute no error, so they are filled with a colour
		// that maps exactly to palette[0].
		std::unordered_map<std::uint32_t, std::uint8_t> cache;
		if (!dither)
		{
			for (std::size_t i = 0; i < pixels; ++i)
			{
				if (!opaque[i])
					continue;
				const std::uint32_t key = Pack(rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2]);
				auto it = cache.find(key);
				if (it == cache.end())
					it = cache.emplace(key, static_cast<std::uint8_t>(Nearest(palette,
						rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2]))).first;
				result.indices[i] = it->second;
			}
		}
		else
		{
			// Floyd-Steinberg, serpentine scan, error carried in two float rows.
			const std::size_t w = static_cast<std::size_t>(width);
			std::vector<float> current((w + 2) * 3, 0.0f), next((w + 2) * 3, 0.0f);
			for (int y = 0; y < height; ++y)
			{
				std::fill(next.begin(), next.end(), 0.0f);
				const bool leftToRight = (y % 2) == 0;
				for (int step = 0; step < width; ++step)
				{
					const int x = leftToRight ? step : width - 1 - step;
					const std::size_t i = static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x);
					if (!opaque[i])
						continue;
					float value[3];
					int clamped[3];
					for (int c = 0; c < 3; ++c)
					{
						value[c] = static_cast<float>(rgba[i * 4 + static_cast<std::size_t>(c)]) +
							current[(static_cast<std::size_t>(x) + 1) * 3 + static_cast<std::size_t>(c)];
						clamped[c] = std::clamp(static_cast<int>(std::lround(value[c])), 0, 255);
					}
					const int best = Nearest(palette, clamped[0], clamped[1], clamped[2]);
					result.indices[i] = static_cast<std::uint8_t>(best);
					const float error[3] = { value[0] - palette[static_cast<std::size_t>(best)].r,
						value[1] - palette[static_cast<std::size_t>(best)].g,
						value[2] - palette[static_cast<std::size_t>(best)].b };
					const int dir = leftToRight ? 1 : -1;
					for (int c = 0; c < 3; ++c)
					{
						const std::size_t cc = static_cast<std::size_t>(c);
						const std::size_t here = (static_cast<std::size_t>(x) + 1) * 3 + cc;
						const std::size_t ahead = (static_cast<std::size_t>(x + dir) + 1) * 3 + cc;
						const std::size_t behind = (static_cast<std::size_t>(x - dir) + 1) * 3 + cc;
						current[ahead] += error[c] * 7.0f / 16.0f;
						next[behind] += error[c] * 3.0f / 16.0f;
						next[here] += error[c] * 5.0f / 16.0f;
						next[ahead] += error[c] * 1.0f / 16.0f;
					}
				}
				current.swap(next);
			}
		}

		if (masked)
		{
			for (std::size_t i = 0; i < pixels; ++i)
			{
				if (!opaque[i])
					result.indices[i] = 255;
			}
			result.palette[255 * 3] = 0;
			result.palette[255 * 3 + 1] = 0;
			result.palette[255 * 3 + 2] = 255;
			result.transparentFraction = static_cast<double>(transparentCount) /
				static_cast<double>(pixels);
		}
		return result;
	}

	std::vector<std::uint8_t> ResizeRgba(const std::uint8_t *rgba, int width, int height,
		int newWidth, int newHeight)
	{
		if (!rgba || width <= 0 || height <= 0 || newWidth <= 0 || newHeight <= 0)
			throw std::runtime_error("resize: bad size");
		std::vector<std::uint8_t> out(static_cast<std::size_t>(newWidth) *
			static_cast<std::size_t>(newHeight) * 4);
		const double sx = static_cast<double>(width) / newWidth;
		const double sy = static_cast<double>(height) / newHeight;
		for (int y = 0; y < newHeight; ++y)
		{
			const double y0 = y * sy;
			const double y1 = std::min<double>(height, y0 + sy);
			for (int x = 0; x < newWidth; ++x)
			{
				const double x0 = x * sx;
				const double x1 = std::min<double>(width, x0 + sx);
				// Area average; alpha-weighted colour so transparent texels do not bleed.
				double accum[4] = { 0, 0, 0, 0 };
				double area = 0.0;
				for (int yy = static_cast<int>(y0); yy < std::min<int>(height, static_cast<int>(std::ceil(y1))); ++yy)
				{
					const double wy = std::min<double>(yy + 1, y1) - std::max<double>(yy, y0);
					for (int xx = static_cast<int>(x0); xx < std::min<int>(width, static_cast<int>(std::ceil(x1))); ++xx)
					{
						const double wx = std::min<double>(xx + 1, x1) - std::max<double>(xx, x0);
						const double weight = wx * wy;
						const std::uint8_t *p = rgba + (static_cast<std::size_t>(yy) *
							static_cast<std::size_t>(width) + static_cast<std::size_t>(xx)) * 4;
						const double alpha = p[3];
						accum[0] += p[0] * alpha * weight;
						accum[1] += p[1] * alpha * weight;
						accum[2] += p[2] * alpha * weight;
						accum[3] += alpha * weight;
						area += weight;
					}
				}
				std::uint8_t *q = &out[(static_cast<std::size_t>(y) *
					static_cast<std::size_t>(newWidth) + static_cast<std::size_t>(x)) * 4];
				if (area <= 0.0 || accum[3] <= 0.0)
				{
					// fully transparent area: keep the nearest source colour
					const int xx = std::min(width - 1, static_cast<int>(x0));
					const int yy = std::min(height - 1, static_cast<int>(y0));
					const std::uint8_t *p = rgba + (static_cast<std::size_t>(yy) *
						static_cast<std::size_t>(width) + static_cast<std::size_t>(xx)) * 4;
					std::memcpy(q, p, 4);
					q[3] = 0;
				}
				else
				{
					q[0] = ClampByte(static_cast<float>(accum[0] / accum[3]));
					q[1] = ClampByte(static_cast<float>(accum[1] / accum[3]));
					q[2] = ClampByte(static_cast<float>(accum[2] / accum[3]));
					q[3] = ClampByte(static_cast<float>(accum[3] / area));
				}
			}
		}
		return out;
	}
}
