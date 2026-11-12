#include "NarCli.h"

#include <cctype>
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

#include "NarArchive.h"
#include "NarTools.h"

namespace fs = std::filesystem;

namespace
{
	using namespace std::string_view_literals;

	struct Args
	{
		std::vector<std::string> positional;
		std::vector<std::string> flags;  // "--name" or "--name=value" already split below
		std::vector<std::pair<std::string, std::string>> options;

		bool Has(const char *flag) const
		{
			for (const auto &f : flags)
				if (f == flag)
					return true;
			return false;
		}
		const std::string *Get(const char *name) const
		{
			for (const auto &o : options)
				if (o.first == name)
					return &o.second;
			return nullptr;
		}
	};

	// Options that take a value; everything else starting with "--" is a flag.
	bool TakesValue(const std::string &name)
	{
		return name == "--type" || name == "--root" || name == "--encrypt-models";
	}

	bool Parse(int argc, char **argv, Args &out)
	{
		for (int i = 2; i < argc; ++i)
		{
			const std::string a = argv[i];
			if (a.rfind("--", 0) != 0)
			{
				out.positional.push_back(a);
				continue;
			}
			if (TakesValue(a))
			{
				if (i + 1 >= argc)
				{
					std::cerr << "Missing value for " << a << "\n";
					return false;
				}
				out.options.emplace_back(a, argv[++i]);
			}
			else
				out.flags.push_back(a);
		}
		return true;
	}

	bool ParseType(const Args &args, cso_pak::NarStoreType &type)
	{
		const std::string *v = args.Get("--type");
		if (v == nullptr)
			return true;
		if (*v == "raw")
			type = cso_pak::NarStoreType::Raw;
		else if (*v == "enc")
			type = cso_pak::NarStoreType::Encoded;
		else if (*v == "lz")
			type = cso_pak::NarStoreType::EncodedCompressed;
		else
		{
			std::cerr << "--type must be raw, enc or lz\n";
			return false;
		}
		return true;
	}

	bool ParseEncrypt(const Args &args, int &version)
	{
		const std::string *v = args.Get("--encrypt-models");
		if (v == nullptr)
			return true;
		if (*v == "20")
			version = 20;
		else if (*v == "21")
			version = 21;
		else
		{
			std::cerr << "--encrypt-models must be 20 or 21\n";
			return false;
		}
		return true;
	}

	cso_pak::NarProgressFn MakeProgress()
	{
		// One-line progress on stderr, throttled to ~10 updates/second.
		auto last = std::make_shared<std::chrono::steady_clock::time_point>();
		return [last](size_t done, size_t total, const std::string &) {
			const auto now = std::chrono::steady_clock::now();
			if (done != total && now - *last < std::chrono::milliseconds(100))
				return;
			*last = now;
			std::cerr << "\r" << done << " / " << total << "      " << std::flush;
			if (done == total)
				std::cerr << "\n";
		};
	}

	void PrintLines(const std::vector<std::string> &lines, const char *prefix, size_t limit = 50)
	{
		for (size_t i = 0; i < lines.size() && i < limit; ++i)
			std::cerr << prefix << lines[i] << "\n";
		if (lines.size() > limit)
			std::cerr << prefix << "... and " << lines.size() - limit << " more\n";
	}

	int List(const Args &args)
	{
		if (args.positional.size() != 1)
			return 2;
		const auto archive = cso_pak::NarArchive::Load(args.positional[0]);
		size_t bad = 0;
		for (const auto &e : archive.Entries())
		{
			static const char *kTypes[] = { "raw", "enc", "lz " };
			std::cout << kTypes[e.type < 3 ? e.type : 0] << "  " << e.extractedSize << "\t" << e.path;
			if (args.Has("--verify") && !archive.VerifyEntry(e))
			{
				std::cout << "\t[CRC MISMATCH]";
				++bad;
			}
			std::cout << "\n";
		}
		std::cerr << archive.Entries().size() << " entries";
		if (args.Has("--verify"))
			std::cerr << ", " << bad << " CRC mismatch(es)";
		std::cerr << "\n";
		return bad == 0 ? 0 : 1;
	}

	int Unpack(const Args &args)
	{
		if (args.positional.empty() || args.positional.size() > 2)
			return 2;
		const fs::path source = args.positional[0];
		fs::path output = args.positional.size() == 2 ? fs::path(args.positional[1])
			: source.parent_path() / source.stem();
		const auto archive = cso_pak::NarArchive::Load(source);

		cso_pak::NarExtractOptions options;
		options.decryptModels = !args.Has("--keep-encrypted");
		options.writeManifest = !args.Has("--no-manifest");
		options.verifyCrc = args.Has("--verify");
		const auto stats = cso_pak::ExtractNarToDirectory(archive, output, options, MakeProgress());

		std::cout << "Unpacked " << stats.written << " of " << stats.total << " entries to " << output.string()
			<< " (" << stats.modelsDecrypted << " ICE model(s) decrypted)\n";
		PrintLines(stats.errors, "  ! ");
		return stats.failed == 0 ? 0 : 1;
	}

	// nar-extract <archive> <text> [output_dir]: only entries whose path
	// contains <text> (case-insensitive). Reads just those entries, so it is
	// fast even on multi-gigabyte archives.
	int Extract(const Args &args)
	{
		if (args.positional.size() < 2 || args.positional.size() > 3)
			return 2;
		std::string needle = args.positional[1];
		for (char &c : needle)
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		const fs::path output = args.positional.size() == 3 ? fs::path(args.positional[2]) : fs::path("nar_extract");
		const auto archive = cso_pak::NarArchive::Load(args.positional[0]);

		cso_pak::NarExtractOptions options;
		options.decryptModels = !args.Has("--keep-encrypted");
		options.verifyCrc = args.Has("--verify");
		options.filter = [needle](const cso_pak::NarArchive::Entry &e) {
			std::string p = e.path;
			for (char &c : p)
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return p.find(needle) != std::string::npos;
		};
		const auto stats = cso_pak::ExtractNarToDirectory(archive, output, options);
		std::cout << "Extracted " << stats.written << " entr" << (stats.written == 1 ? "y" : "ies") << " to "
			<< output.string() << " (" << stats.modelsDecrypted << " ICE model(s) decrypted)\n";
		PrintLines(stats.errors, "  ! ");
		return stats.failed == 0 && stats.written > 0 ? 0 : 1;
	}

	int Pack(const Args &args)
	{
		if (args.positional.size() != 2)
			return 2;
		cso_pak::NarPackOptions options;
		if (!ParseType(args, options.defaultType) || !ParseEncrypt(args, options.encryptModels))
			return 2;
		if (const std::string *root = args.Get("--root"))
		{
			options.hasRoot = true;
			options.root = *root;
		}
		options.useManifest = !args.Has("--no-manifest");

		const auto stats = cso_pak::PackDirectoryToNar(args.positional[0], args.positional[1], options, MakeProgress());
		std::cout << "Packed " << stats.files << " file(s) into " << args.positional[1]
			<< (stats.usedManifest ? " (settings restored from .nar-info.json)" : "") << "; "
			<< stats.modelsEncrypted << " model(s) ICE-encrypted\n";
		PrintLines(stats.warnings, "  ! ");
		return 0;
	}

	int Patch(const Args &args)
	{
		if (args.positional.size() != 3)
			return 2;
		cso_pak::NarPatchOptions options;
		if (!ParseType(args, options.newFileType) || !ParseEncrypt(args, options.encryptNewModels))
			return 2;
		options.addNew = args.Has("--add-new");
		if (const std::string *root = args.Get("--root"))
			options.root = *root;
		if (options.addNew && args.Get("--root") == nullptr)
		{
			std::cerr << "--add-new needs --root <name> (where new files go in the archive)\n";
			return 2;
		}

		const auto source = cso_pak::NarArchive::Load(args.positional[0]);
		const auto stats = cso_pak::PatchNar(source, args.positional[1], args.positional[2], options, MakeProgress());
		std::cout << "Patched: " << stats.replaced << " replaced, " << stats.added << " added, "
			<< stats.preserved << " copied unchanged\n";
		PrintLines(stats.skipped, "  skipped: ");
		return 0;
	}
}

namespace cso_pak
{
	void PrintNarUsage(const char *programName)
	{
		std::cout
			<< "  " << programName << " nar-list <archive.nar> [--verify]\n"
			<< "  " << programName << " nar-unpack <archive.nar> [output_dir] [--keep-encrypted] [--verify] [--no-manifest]\n"
			<< "  " << programName << " nar-extract <archive.nar> <path_text> [output_dir] [--keep-encrypted] [--verify]\n"
			<< "  " << programName << " nar-pack <input_dir> <output.nar> [--type raw|enc|lz] [--root NAME]\n"
			<< "        [--encrypt-models 20|21] [--no-manifest]\n"
			<< "  " << programName << " nar-patch <source.nar> <replacement_dir> <output.nar> [--type raw|enc|lz]\n"
			<< "        [--add-new --root NAME] [--encrypt-models 20|21]\n\n";
	}

	int RunNarCommand(std::string_view command, int argc, char **argv)
	{
		int (*handler)(const Args &) = nullptr;
		if (command == "nar-list"sv) handler = List;
		else if (command == "nar-unpack"sv) handler = Unpack;
		else if (command == "nar-extract"sv) handler = Extract;
		else if (command == "nar-pack"sv) handler = Pack;
		else if (command == "nar-patch"sv) handler = Patch;
		else return -1;

		Args args;
		if (!Parse(argc, argv, args))
			return 2;
		return handler(args);
	}
}
