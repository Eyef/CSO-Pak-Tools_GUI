#include "NarTools.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_map>

#include "ModelCrypt.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace cso_pak
{
	namespace
	{
		// ------------------------------------------------------------ text

		std::string LowerAscii(std::string s)
		{
			for (char &c : s)
				if (c >= 'A' && c <= 'Z')
					c = static_cast<char>(c - 'A' + 'a');
			return s;
		}

		bool EndsWithNoCase(const std::string &s, const char *suffix)
		{
			const size_t n = std::strlen(suffix);
			return s.size() >= n && LowerAscii(s.substr(s.size() - n)) == suffix;
		}

		bool IsMdlPath(const std::string &path) { return EndsWithNoCase(path, ".mdl"); }

		// UTF-8 text of a path with '/' separators (generic form).
		std::string Utf8Generic(const fs::path &p)
		{
			const auto u = p.generic_u8string();
			return std::string(reinterpret_cast<const char *>(u.data()), u.size());
		}

		fs::path PathFromUtf8(const std::string &s)
		{
			return fs::path(std::u8string(reinterpret_cast<const char8_t *>(s.data()), s.size()));
		}

		bool IsReservedWindowsName(std::string name)
		{
			const size_t dot = name.find('.');
			if (dot != std::string::npos)
				name.resize(dot);
			name = LowerAscii(name);
			if (name == "con" || name == "prn" || name == "aux" || name == "nul")
				return true;
			return name.size() == 4 && (name.rfind("com", 0) == 0 || name.rfind("lpt", 0) == 0) &&
				name[3] >= '1' && name[3] <= '9';
		}

		// ----------------------------------------------- code page (Windows)

#ifdef _WIN32
		// Archive paths are EUC-KR (code page 949); local paths are UTF-16.
		bool HasHighBit(const std::string &s)
		{
			return std::any_of(s.begin(), s.end(), [](char c) { return static_cast<unsigned char>(c) >= 0x80; });
		}

		std::wstring ArchiveBytesToWide(const std::string &s)
		{
			if (s.empty())
				return {};
			const int n = MultiByteToWideChar(949, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
			if (n <= 0)
				return {};
			std::wstring w(static_cast<size_t>(n), L'\0');
			MultiByteToWideChar(949, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), w.data(), n);
			return w;
		}

		std::string WideToArchiveBytes(const std::wstring &w, bool &ok)
		{
			ok = true;
			if (w.empty())
				return {};
			BOOL usedDefault = FALSE;
			const int n = WideCharToMultiByte(949, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, &usedDefault);
			if (n <= 0 || usedDefault)
			{
				ok = false;
				return {};
			}
			std::string s(static_cast<size_t>(n), '\0');
			usedDefault = FALSE;
			WideCharToMultiByte(949, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, &usedDefault);
			ok = !usedDefault;
			return ok ? s : std::string();
		}
#endif

		// ------------------------------------------------------------ time

		uint32_t FileTimeToUnix(const fs::path &p)
		{
			try
			{
				const auto ft = fs::last_write_time(p);
				const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(ft);
				const auto secs = std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
				if (secs < 0)
					return 0;
				return secs > 0xFFFFFFFFll ? 0xFFFFFFFFu : static_cast<uint32_t>(secs);
			}
			catch (...)
			{
				return 0;
			}
		}

		void SetFileTimeFromUnix(const fs::path &p, uint32_t unixSeconds)
		{
			if (unixSeconds == 0)
				return;
			try
			{
				const std::chrono::sys_seconds sys{ std::chrono::seconds(unixSeconds) };
				fs::last_write_time(p, std::chrono::clock_cast<fs::file_time_type::clock>(sys));
			}
			catch (...)
			{
			}  // cosmetic only
		}

		// -------------------------------------------------------- tiny JSON

		struct Json
		{
			enum Type { Null, Bool, Number, String, Array, Object } type = Null;
			bool boolean = false;
			double number = 0;
			std::string str;
			std::vector<Json> array;
			std::vector<std::pair<std::string, Json>> object;

			const Json *Find(const char *key) const
			{
				for (const auto &kv : object)
					if (LowerAscii(kv.first) == LowerAscii(key))
						return &kv.second;
				return nullptr;
			}
		};

		class JsonParser
		{
		public:
			explicit JsonParser(const std::string &text) : s_(text) {}

			bool Parse(Json &out)
			{
				try
				{
					SkipWs();
					out = Value(0);
					SkipWs();
					return pos_ == s_.size();
				}
				catch (const std::exception &)
				{
					return false;
				}
			}

		private:
			void SkipWs()
			{
				while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r'))
					++pos_;
				// tolerate a UTF-8 BOM at the very start
				if (pos_ == 0 && s_.size() >= 3 && static_cast<unsigned char>(s_[0]) == 0xEF &&
					static_cast<unsigned char>(s_[1]) == 0xBB && static_cast<unsigned char>(s_[2]) == 0xBF)
				{
					pos_ = 3;
					SkipWs();
				}
			}

			[[noreturn]] void Fail() { throw std::runtime_error("json"); }

			char Peek()
			{
				if (pos_ >= s_.size())
					Fail();
				return s_[pos_];
			}

			void Expect(char c)
			{
				if (Peek() != c)
					Fail();
				++pos_;
			}

			bool Literal(const char *word)
			{
				const size_t n = std::strlen(word);
				if (s_.compare(pos_, n, word) == 0)
				{
					pos_ += n;
					return true;
				}
				return false;
			}

			Json Value(int depth)
			{
				if (depth > 32)
					Fail();
				Json v;
				const char c = Peek();
				if (c == '{')
				{
					++pos_;
					v.type = Json::Object;
					SkipWs();
					if (Peek() == '}')
					{
						++pos_;
						return v;
					}
					for (;;)
					{
						SkipWs();
						std::string key = String();
						SkipWs();
						Expect(':');
						SkipWs();
						v.object.emplace_back(std::move(key), Value(depth + 1));
						SkipWs();
						if (Peek() == ',')
						{
							++pos_;
							continue;
						}
						Expect('}');
						return v;
					}
				}
				if (c == '[')
				{
					++pos_;
					v.type = Json::Array;
					SkipWs();
					if (Peek() == ']')
					{
						++pos_;
						return v;
					}
					for (;;)
					{
						SkipWs();
						v.array.push_back(Value(depth + 1));
						SkipWs();
						if (Peek() == ',')
						{
							++pos_;
							continue;
						}
						Expect(']');
						return v;
					}
				}
				if (c == '"')
				{
					v.type = Json::String;
					v.str = String();
					return v;
				}
				if (Literal("true"))
				{
					v.type = Json::Bool;
					v.boolean = true;
					return v;
				}
				if (Literal("false"))
				{
					v.type = Json::Bool;
					return v;
				}
				if (Literal("null"))
					return v;
				// number
				const size_t start = pos_;
				while (pos_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '-' ||
					s_[pos_] == '+' || s_[pos_] == '.' || s_[pos_] == 'e' || s_[pos_] == 'E'))
					++pos_;
				if (pos_ == start)
					Fail();
				v.type = Json::Number;
				v.number = std::strtod(s_.substr(start, pos_ - start).c_str(), nullptr);
				return v;
			}

			static void AppendUtf8(std::string &out, uint32_t cp)
			{
				if (cp < 0x80)
					out.push_back(static_cast<char>(cp));
				else if (cp < 0x800)
				{
					out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
					out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
				}
				else if (cp < 0x10000)
				{
					out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
					out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
					out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
				}
				else
				{
					out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
					out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
					out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
					out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
				}
			}

			uint32_t Hex4()
			{
				if (pos_ + 4 > s_.size())
					Fail();
				uint32_t v = 0;
				for (int i = 0; i < 4; ++i)
				{
					const char h = s_[pos_++];
					v <<= 4;
					if (h >= '0' && h <= '9') v |= static_cast<uint32_t>(h - '0');
					else if (h >= 'a' && h <= 'f') v |= static_cast<uint32_t>(h - 'a' + 10);
					else if (h >= 'A' && h <= 'F') v |= static_cast<uint32_t>(h - 'A' + 10);
					else Fail();
				}
				return v;
			}

			std::string String()
			{
				Expect('"');
				std::string out;
				for (;;)
				{
					const char c = Peek();
					++pos_;
					if (c == '"')
						return out;
					if (c != '\\')
					{
						out.push_back(c);
						continue;
					}
					const char e = Peek();
					++pos_;
					switch (e)
					{
					case '"': out.push_back('"'); break;
					case '\\': out.push_back('\\'); break;
					case '/': out.push_back('/'); break;
					case 'b': out.push_back('\b'); break;
					case 'f': out.push_back('\f'); break;
					case 'n': out.push_back('\n'); break;
					case 'r': out.push_back('\r'); break;
					case 't': out.push_back('\t'); break;
					case 'u':
					{
						uint32_t cp = Hex4();
						if (cp >= 0xD800 && cp < 0xDC00 && s_.compare(pos_, 2, "\\u") == 0)
						{
							pos_ += 2;
							const uint32_t lo = Hex4();
							if (lo >= 0xDC00 && lo < 0xE000)
								cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
						}
						AppendUtf8(out, cp);
						break;
					}
					default: Fail();
					}
				}
			}

			const std::string &s_;
			size_t pos_ = 0;
		};

		std::string JsonEscape(const std::string &s)
		{
			std::string out = "\"";
			for (const unsigned char c : s)
			{
				switch (c)
				{
				case '"': out += "\\\""; break;
				case '\\': out += "\\\\"; break;
				case '\n': out += "\\n"; break;
				case '\r': out += "\\r"; break;
				case '\t': out += "\\t"; break;
				default:
					if (c < 0x20)
					{
						char buf[8];
						std::snprintf(buf, sizeof(buf), "\\u%04x", c);
						out += buf;
					}
					else
						out.push_back(static_cast<char>(c));
				}
			}
			out += '"';
			return out;
		}

		// ---------------------------------------------------------- manifest

		struct ManifestEntry
		{
			std::string path;  // "/cstrike/models/a.mdl", UTF-8
			int store = 1;
			int mdl = 0;       // 20/21 if the model was ICE-encrypted
			int64_t ord = -1;  // directory position
			int64_t off = -1;  // data offset
		};

		struct Manifest
		{
			std::vector<ManifestEntry> entries;
			std::unordered_map<std::string, size_t> byKey;  // lower-case, no leading '/'

			static std::string KeyOf(const std::string &path)
			{
				size_t i = 0;
				while (i < path.size() && path[i] == '/')
					++i;
				return LowerAscii(path.substr(i));
			}

			void Index()
			{
				byKey.clear();
				for (size_t i = 0; i < entries.size(); ++i)
					byKey[KeyOf(entries[i].path)] = i;
			}

			const ManifestEntry *Find(const std::string &relativePath) const
			{
				const auto it = byKey.find(KeyOf(relativePath));
				return it == byKey.end() ? nullptr : &entries[it->second];
			}
		};

		void SaveManifest(const fs::path &dir, const std::string &source, const std::vector<ManifestEntry> &entries)
		{
			try
			{
				char stamp[32] = "";
				const std::time_t now = std::time(nullptr);
				std::tm tmv{};
#ifdef _WIN32
				localtime_s(&tmv, &now);
#else
				localtime_r(&now, &tmv);
#endif
				std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &tmv);

				std::string json = "{\n  \"Version\": 1,\n  \"Source\": " + JsonEscape(source) +
					",\n  \"Created\": " + JsonEscape(stamp) + ",\n  \"Entries\": [\n";
				for (size_t i = 0; i < entries.size(); ++i)
				{
					const ManifestEntry &e = entries[i];
					json += "    { \"Path\": " + JsonEscape(e.path) + ", \"Store\": " + std::to_string(e.store);
					if (e.mdl != 0)
						json += ", \"Mdl\": " + std::to_string(e.mdl);
					json += ", \"Ord\": " + std::to_string(e.ord) + ", \"Off\": " + std::to_string(e.off) + " }";
					json += i + 1 < entries.size() ? ",\n" : "\n";
				}
				json += "  ]\n}\n";

				std::ofstream f(dir / kNarManifestName, std::ios::binary | std::ios::trunc);
				f.write(json.data(), static_cast<std::streamsize>(json.size()));
			}
			catch (...)
			{
			}  // the manifest is a convenience: never fail the unpack because of it
		}

		std::optional<Manifest> LoadManifest(const fs::path &dir)
		{
			try
			{
				const fs::path file = dir / kNarManifestName;
				std::error_code ec;
				if (!fs::is_regular_file(file, ec) || fs::file_size(file, ec) > (256u << 20))
					return std::nullopt;
				std::ifstream f(file, std::ios::binary);
				const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
				Json root;
				if (!JsonParser(text).Parse(root) || root.type != Json::Object)
					return std::nullopt;
				const Json *version = root.Find("Version");
				if (version == nullptr || version->type != Json::Number || version->number != 1)
					return std::nullopt;
				const Json *entries = root.Find("Entries");
				if (entries == nullptr || entries->type != Json::Array)
					return std::nullopt;

				Manifest m;
				for (const Json &e : entries->array)
				{
					if (e.type != Json::Object)
						continue;
					const Json *path = e.Find("Path");
					if (path == nullptr || path->type != Json::String || path->str.empty())
						continue;
					ManifestEntry me;
					me.path = path->str;
					if (const Json *v = e.Find("Store"); v && v->type == Json::Number)
						me.store = static_cast<int>(v->number);
					if (const Json *v = e.Find("Mdl"); v && v->type == Json::Number)
						me.mdl = static_cast<int>(v->number);
					if (const Json *v = e.Find("Ord"); v && v->type == Json::Number)
						me.ord = static_cast<int64_t>(v->number);
					if (const Json *v = e.Find("Off"); v && v->type == Json::Number)
						me.off = static_cast<int64_t>(v->number);
					if (me.store < 0 || me.store > 2)
						me.store = 1;
					if (me.mdl != 20 && me.mdl != 21)
						me.mdl = 0;
					m.entries.push_back(std::move(me));
				}
				m.Index();
				return m;
			}
			catch (...)
			{
				return std::nullopt;
			}
		}

		// ------------------------------------------------------- file utils

		std::vector<uint8_t> ReadFile(const fs::path &p)
		{
			std::ifstream f(p, std::ios::binary | std::ios::ate);
			if (!f)
				throw std::runtime_error("cannot open " + Utf8Generic(p));
			const std::streamoff size = f.tellg();
			if (size < 0 || size > 0x7FFFFFFF)
				throw std::runtime_error("file too large for a .nar entry: " + Utf8Generic(p));
			f.seekg(0);
			std::vector<uint8_t> data(static_cast<size_t>(size));
			if (size > 0 && !f.read(reinterpret_cast<char *>(data.data()), size))
				throw std::runtime_error("cannot read " + Utf8Generic(p));
			return data;
		}

		void WriteFile(const fs::path &p, const std::vector<uint8_t> &data)
		{
			std::ofstream f(p, std::ios::binary | std::ios::trunc);
			if (!f)
				throw std::runtime_error("cannot create " + Utf8Generic(p));
			if (!data.empty())
				f.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
			f.flush();
			if (!f)
				throw std::runtime_error("write failed: " + Utf8Generic(p));
		}

		struct LocalFile
		{
			fs::path absolute;
			fs::path relative;  // relative to the scanned folder
			std::string relKey;  // generic UTF-8 form
		};

		std::vector<LocalFile> CollectFiles(const fs::path &root)
		{
			std::vector<LocalFile> files;
			const auto options = fs::directory_options::skip_permission_denied;
			for (const auto &entry : fs::recursive_directory_iterator(root, options))
			{
				if (entry.is_symlink() || !entry.is_regular_file())
					continue;
				if (entry.path().filename() == kNarManifestName)
					continue;
				LocalFile f;
				f.absolute = entry.path();
				f.relative = fs::relative(entry.path(), root);
				f.relKey = Utf8Generic(f.relative);
				files.push_back(std::move(f));
			}
			std::sort(files.begin(), files.end(), [](const LocalFile &a, const LocalFile &b) {
				return LowerAscii(a.relKey) < LowerAscii(b.relKey);
			});
			return files;
		}
	}

	// =================================================================== paths

	fs::path ArchivePathToLocal(const std::string &archivePath)
	{
		std::vector<std::string> parts;
		std::string cur;
		const auto flush = [&] {
			if (!cur.empty() && cur != ".")
				parts.push_back(cur);
			cur.clear();
		};
		for (const char c : archivePath)
		{
			if (c == '/' || c == '\\')
				flush();
			else
				cur.push_back(c);
		}
		flush();
		if (parts.empty())
			return {};

		fs::path result;
		for (const std::string &part : parts)
		{
			if (part == "..")
				return {};
			for (const unsigned char c : part)
			{
				if (c < 0x20 || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
					return {};
			}
			if (part.back() == '.' || part.back() == ' ' || IsReservedWindowsName(part))
				return {};
#ifdef _WIN32
			if (HasHighBit(part))
			{
				const std::wstring wide = ArchiveBytesToWide(part);
				if (wide.empty())
					return {};
				result /= fs::path(wide);
				continue;
			}
#endif
			result /= fs::path(part);
		}
		return result;
	}

	std::string LocalToArchivePath(const fs::path &relative)
	{
		std::string out;
		for (const fs::path &part : relative)
		{
			if (part.empty() || part == "/" || part == "\\")
				continue;
#ifdef _WIN32
			bool ok = true;
			const std::string bytes = WideToArchiveBytes(part.wstring(), ok);
			if (!ok)
				return {};
#else
			const std::string bytes = part.string();
#endif
			out += '/';
			out += bytes;
		}
		return out;
	}

	// ================================================================= unpack

	NarExtractStats ExtractNarToDirectory(const NarArchive &archive, const fs::path &outputDir,
		const NarExtractOptions &options, const NarProgressFn &progress)
	{
		NarExtractStats stats;
		fs::create_directories(outputDir);

		const auto &entries = archive.Entries();
		stats.total = entries.size();
		std::vector<ManifestEntry> manifest;
		manifest.reserve(entries.size());
		std::set<std::string> seen;

		for (size_t i = 0; i < entries.size(); ++i)
		{
			const NarArchive::Entry &entry = entries[i];
			if (options.filter && !options.filter(entry))
			{
				--stats.total;
				continue;
			}
			if (progress)
				progress(i, entries.size(), entry.path);
			try
			{
				const fs::path local = ArchivePathToLocal(entry.path);
				if (local.empty())
					throw std::runtime_error("unsafe or unrepresentable path");
				const std::string localKey = Utf8Generic(local);
				if (!seen.insert(LowerAscii(localKey)).second)
					throw std::runtime_error("duplicate path (case-insensitive), kept the first");

				std::vector<uint8_t> data = archive.ExtractEntry(entry);
				if (options.verifyCrc && !archive.VerifyEntry(entry))
					stats.errors.push_back(entry.path + ": CRC mismatch (file still written)");

				int modelVersion = 0;
				if (options.decryptModels && IsMdlPath(entry.path))
				{
					int v = 0;
					if (ModelCrypt::Decrypt(data, &v) == ModelCrypt::Result::Success)
					{
						modelVersion = v;
						++stats.modelsDecrypted;
					}
				}

				const fs::path target = outputDir / local;
				fs::create_directories(target.parent_path());
				WriteFile(target, data);
				SetFileTimeFromUnix(target, entry.timestamp);

				ManifestEntry me;
				me.path = "/" + localKey;
				me.store = static_cast<int>(entry.type);
				me.mdl = modelVersion;
				me.ord = static_cast<int64_t>(i);
				me.off = static_cast<int64_t>(entry.offset);
				manifest.push_back(std::move(me));
				++stats.written;
			}
			catch (const std::exception &e)
			{
				++stats.failed;
				stats.errors.push_back(entry.path + ": " + e.what());
			}
		}
		if (progress)
			progress(entries.size(), entries.size(), {});

		if (options.writeManifest && !options.filter)
			SaveManifest(outputDir, Utf8Generic(archive.SourcePath()), manifest);
		return stats;
	}

	// =================================================================== pack

	NarPackStats PackDirectoryToNar(const fs::path &inputDir, const fs::path &outputNar,
		const NarPackOptions &options, const NarProgressFn &progress)
	{
		if (!fs::is_directory(inputDir))
			throw std::runtime_error("not a directory: " + Utf8Generic(inputDir));

		NarPackStats stats;
		const fs::path absInput = fs::absolute(inputDir).lexically_normal();

		// Manifest: in the folder itself or up to two levels above it.
		std::optional<Manifest> manifest;
		std::string manifestPrefix;  // path of the input folder below the manifest folder
		if (options.useManifest)
		{
			fs::path dir = absInput;
			if (!dir.has_filename())
				dir = dir.parent_path();
			for (int depth = 0; depth < 3 && !dir.empty(); ++depth)
			{
				manifest = LoadManifest(dir);
				if (manifest)
				{
					const std::string rel = Utf8Generic(absInput.lexically_relative(dir));
					manifestPrefix = (rel == "." || rel.empty()) ? std::string() : rel + "/";
					break;
				}
				const fs::path parent = dir.parent_path();
				if (parent == dir)
					break;
				dir = parent;
			}
		}
		stats.usedManifest = manifest.has_value();

		std::string rootPrefix;  // used when there is no manifest
		if (!manifest)
		{
			if (options.hasRoot)
			{
				rootPrefix = options.root;
				while (!rootPrefix.empty() && (rootPrefix.front() == '/' || rootPrefix.front() == '\\'))
					rootPrefix.erase(rootPrefix.begin());
				while (!rootPrefix.empty() && (rootPrefix.back() == '/' || rootPrefix.back() == '\\'))
					rootPrefix.pop_back();
			}
			else
			{
				fs::path name = absInput.has_filename() ? absInput.filename() : absInput.parent_path().filename();
				rootPrefix = Utf8Generic(name);
			}
			if (!rootPrefix.empty())
				rootPrefix += "/";
		}

		struct Job
		{
			LocalFile file;
			std::string archivePath;
			const ManifestEntry *known = nullptr;
		};
		std::vector<Job> jobs;
		const fs::path absOutput = fs::absolute(outputNar).lexically_normal();
		for (LocalFile &f : CollectFiles(absInput))
		{
			if (f.absolute.lexically_normal() == absOutput)
				continue;  // never pack the archive into itself
			const std::string keyPath = (manifest ? manifestPrefix : rootPrefix) + f.relKey;
			Job j;
			j.archivePath = LocalToArchivePath(PathFromUtf8(keyPath));
			if (j.archivePath.empty() || j.archivePath.size() > 0xFFFF)
			{
				stats.warnings.push_back("skipped (name can't be stored in a .nar): " + f.relKey);
				continue;
			}
			if (manifest)
				j.known = manifest->Find(keyPath);
			j.file = std::move(f);
			jobs.push_back(std::move(j));
		}
		if (jobs.empty())
			throw std::runtime_error("no files to pack in " + Utf8Generic(inputDir));

		// Data blocks in the original order where known (by offset), the rest
		// afterwards; the directory table follows the original order too.
		std::stable_sort(jobs.begin(), jobs.end(), [](const Job &a, const Job &b) {
			const bool ka = a.known != nullptr && a.known->off >= 0;
			const bool kb = b.known != nullptr && b.known->off >= 0;
			if (ka != kb)
				return ka;
			if (ka)
				return a.known->off < b.known->off;
			return LowerAscii(a.archivePath) < LowerAscii(b.archivePath);
		});

		NarWriter writer(outputNar);
		size_t knownCount = 0;
		for (size_t i = 0; i < jobs.size(); ++i)
		{
			const Job &job = jobs[i];
			if (progress)
				progress(i, jobs.size(), job.archivePath);

			std::vector<uint8_t> data = ReadFile(job.file.absolute);

			// Known entries keep their original model encryption (or lack of it);
			// unknown ones follow the option.
			const int modelVersion = job.known != nullptr ? job.known->mdl : options.encryptModels;
			if (modelVersion != 0 && IsMdlPath(job.archivePath) && ModelCrypt::EncryptedVersion(data) == 0)
			{
				if (ModelCrypt::Encrypt(data, modelVersion) == ModelCrypt::Result::Success)
					++stats.modelsEncrypted;
				else
					stats.warnings.push_back("model left unencrypted (not a valid studio model?): " + job.archivePath);
			}

			NarStoreType type = options.defaultType;
			uint64_t order = UINT64_MAX;
			if (job.known != nullptr)
			{
				type = static_cast<NarStoreType>(job.known->store);
				if (job.known->ord >= 0)
					order = static_cast<uint64_t>(job.known->ord);
				++knownCount;
			}
			writer.AddFile(job.archivePath, data, type, FileTimeToUnix(job.file.absolute), order);
		}
		if (progress)
			progress(jobs.size(), jobs.size(), {});
		writer.Finish();

		stats.files = jobs.size();
		if (manifest && manifestPrefix.empty() && knownCount < manifest->entries.size())
			stats.warnings.push_back(std::to_string(manifest->entries.size() - knownCount) +
				" file(s) listed in " + kNarManifestName + " are missing and were not packed");
		return stats;
	}

	// =================================================================== patch

	NarPatchStats PatchNar(const NarArchive &source, const fs::path &replacementDir,
		const fs::path &outputNar, const NarPatchOptions &options, const NarProgressFn &progress)
	{
		if (!fs::is_directory(replacementDir))
			throw std::runtime_error("not a directory: " + Utf8Generic(replacementDir));
		std::error_code ec;
		if (fs::exists(outputNar, ec) && fs::equivalent(outputNar, source.SourcePath(), ec))
			throw std::runtime_error("output must differ from the source archive");

		NarPatchStats stats;
		const auto &entries = source.Entries();
		const size_t npos = static_cast<size_t>(-1);

		std::unordered_map<std::string, size_t> byPath;
		std::unordered_map<std::string, std::vector<size_t>> byName;
		for (size_t i = 0; i < entries.size(); ++i)
		{
			const std::string lower = LowerAscii(entries[i].path);
			byPath.emplace(lower, i);
			const size_t slash = lower.find_last_of('/');
			byName[slash == std::string::npos ? lower : lower.substr(slash + 1)].push_back(i);
		}

		struct Replacement
		{
			LocalFile file;
			size_t target = npos;  // entry index, or npos for a new file
			std::string newPath;
		};
		std::vector<Replacement> repls;
		std::vector<size_t> replFor(entries.size(), npos);

		std::string root = options.root;
		while (!root.empty() && (root.front() == '/' || root.front() == '\\'))
			root.erase(root.begin());
		while (!root.empty() && (root.back() == '/' || root.back() == '\\'))
			root.pop_back();

		for (LocalFile &f : CollectFiles(replacementDir))
		{
			const std::string arch = LocalToArchivePath(f.relative);
			if (arch.empty())
			{
				stats.skipped.push_back(f.relKey + ": name can't be stored in a .nar");
				continue;
			}
			const std::string lower = LowerAscii(arch);
			size_t target = npos;
			if (const auto it = byPath.find(lower); it != byPath.end())
			{
				target = it->second;
			}
			else
			{
				const size_t slash = lower.find_last_of('/');
				const std::string name = slash == std::string::npos ? lower : lower.substr(slash + 1);
				std::vector<size_t> matches;
				if (const auto it = byName.find(name); it != byName.end())
				{
					for (const size_t idx : it->second)
					{
						const std::string cand = LowerAscii(entries[idx].path);
						if (cand.size() > lower.size() && cand.compare(cand.size() - lower.size(), lower.size(), lower) == 0)
							matches.push_back(idx);
					}
				}
				if (matches.size() > 1)
				{
					stats.skipped.push_back(f.relKey + ": ambiguous, " + std::to_string(matches.size()) +
						" archive entries end with this path (use the full path from the archive root)");
					continue;
				}
				if (matches.size() == 1)
					target = matches[0];
			}

			Replacement r;
			r.target = target;
			if (target != npos)
			{
				if (replFor[target] != npos)
				{
					stats.skipped.push_back(f.relKey + ": another file already replaces " + entries[target].path);
					continue;
				}
				replFor[target] = repls.size();
			}
			else if (options.addNew)
			{
				r.newPath = root.empty() ? arch : "/" + root + arch;
			}
			else
			{
				stats.skipped.push_back(f.relKey + ": no matching entry in the archive");
				continue;
			}
			r.file = std::move(f);
			repls.push_back(std::move(r));
		}

		NarWriter writer(outputNar);
		for (size_t i = 0; i < entries.size(); ++i)
		{
			const NarArchive::Entry &e = entries[i];
			if (progress)
				progress(i, entries.size(), e.path);
			if (replFor[i] == npos)
			{
				writer.AddStored(e.path, e, source.ReadStored(e), i);
				++stats.preserved;
				continue;
			}
			const Replacement &r = repls[replFor[i]];
			std::vector<uint8_t> data = ReadFile(r.file.absolute);
			if (IsMdlPath(e.path) && ModelCrypt::EncryptedVersion(data) == 0)
			{
				// The old model was ICE-encrypted? Then the new one must be too.
				try
				{
					const int version = ModelCrypt::EncryptedVersion(source.ExtractEntry(e));
					if (version != 0)
						ModelCrypt::Encrypt(data, version);
				}
				catch (const std::exception &)
				{
				}
			}
			writer.AddFile(e.path, data, static_cast<NarStoreType>(e.type), FileTimeToUnix(r.file.absolute), i);
			++stats.replaced;
		}
		for (const Replacement &r : repls)
		{
			if (r.target != npos)
				continue;
			std::vector<uint8_t> data = ReadFile(r.file.absolute);
			if (options.encryptNewModels != 0 && IsMdlPath(r.newPath) && ModelCrypt::EncryptedVersion(data) == 0)
				ModelCrypt::Encrypt(data, options.encryptNewModels);
			writer.AddFile(r.newPath, data, options.newFileType, FileTimeToUnix(r.file.absolute));
			++stats.added;
		}
		if (progress)
			progress(entries.size(), entries.size(), {});
		writer.Finish();
		return stats;
	}
}
