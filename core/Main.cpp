#include "PakArchive.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	using namespace std::string_view_literals;

	std::string ProgramName(const char *program)
	{
		const auto filename = std::filesystem::path(program).filename().string();
		if (!filename.empty())
			return filename;

		return "cso-pak-tool";
	}

	void PrintUsage(const char *program)
	{
		const auto programName = ProgramName(program);

		std::cout
			<< "Usage:\n"
			<< "  " << programName << " unpack <source.pak> [output_root]\n"
			<< "  " << programName << " unpack <source_dir> [output_root]\n"
			<< "  " << programName << " pack <input_dir> <output.pak>\n"
			<< "  " << programName << " patch <source.pak> <replacement_dir> <output.pak>\n\n";
	}

	bool IsPakFile(const std::filesystem::path &path)
	{
		auto extension = path.extension().string();
		std::ranges::transform(extension, extension.begin(), [](unsigned char ch) {
			return static_cast<char>(std::tolower(ch));
		});

		return extension == ".pak";
	}

	std::filesystem::path DefaultUnpackOutputRoot(const std::filesystem::path &sourcePak)
	{
		const auto parent = sourcePak.parent_path();
		if (parent.empty())
			return ".";

		return parent;
	}

	cso_pak::UnpackStats UnpackOne(const std::filesystem::path &sourcePak,
		const std::filesystem::path &outputRoot)
	{
		const auto archive = cso_pak::PakArchive::Load(sourcePak);
		const auto stats = archive.UnpackToDirectory(outputRoot);

		std::cout << "Unpacked " << sourcePak << " -> " << outputRoot << "\n"
			<< "Entries: " << stats.totalEntries
			<< ", written: " << stats.writtenEntries << "\n";

		return stats;
	}

	std::vector<std::filesystem::path> CollectPakFiles(const std::filesystem::path &sourceDir)
	{
		std::vector<std::filesystem::path> pakFiles;
		const auto options = std::filesystem::directory_options::skip_permission_denied;

		for (const auto &entry : std::filesystem::recursive_directory_iterator(sourceDir, options))
		{
			if (entry.is_regular_file() && IsPakFile(entry.path()))
				pakFiles.push_back(entry.path());
		}

		std::ranges::sort(pakFiles);
		return pakFiles;
	}

	std::filesystem::path UnpackOutputRoot(const std::filesystem::path &sourcePak,
		const std::filesystem::path &outputRoot)
	{
		if (outputRoot.empty())
			return DefaultUnpackOutputRoot(sourcePak);

		return outputRoot;
	}

	int RunUnpack(int argc, char **argv)
	{
		if (argc != 3 && argc != 4)
			return 2;

		const std::filesystem::path source = argv[2];
		const std::filesystem::path output = argc == 4 ? std::filesystem::path(argv[3]) :
			std::filesystem::path();

		if (!std::filesystem::is_directory(source))
		{
			UnpackOne(source, UnpackOutputRoot(source, output));
			return 0;
		}

		const auto pakFiles = CollectPakFiles(source);
		if (pakFiles.empty())
			throw std::runtime_error("no .pak files found in source directory");

		size_t totalEntries = 0;
		size_t writtenEntries = 0;
		for (const auto &pakFile : pakFiles)
		{
			const auto stats = UnpackOne(pakFile, UnpackOutputRoot(pakFile, output));
			totalEntries += stats.totalEntries;
			writtenEntries += stats.writtenEntries;
		}

		std::cout << "Batch unpacked " << pakFiles.size() << " pak file(s)\n"
			<< "Entries: " << totalEntries
			<< ", written: " << writtenEntries << "\n";
		return 0;
	}

	int RunPack(int argc, char **argv)
	{
		if (argc != 4)
			return 2;

		const std::filesystem::path inputDir = argv[2];
		const std::filesystem::path outputPak = argv[3];

		const auto stats = cso_pak::PakArchive::PackDirectory(inputDir, outputPak);

		std::cout << "Wrote " << outputPak << "\n"
			<< "Entries: " << stats.totalEntries
			<< ", packed: " << stats.packedEntries << "\n";
		return 0;
	}

	int RunPatch(int argc, char **argv)
	{
		if (argc != 5)
			return 2;

		const std::filesystem::path sourcePak = argv[2];
		const std::filesystem::path replacementDir = argv[3];
		const std::filesystem::path outputPak = argv[4];

		const auto archive = cso_pak::PakArchive::Load(sourcePak);
		const auto stats = archive.PatchFromDirectory(replacementDir, outputPak);

		std::cout << "Wrote " << outputPak << "\n"
			<< "Entries: " << stats.totalEntries
			<< ", replaced: " << stats.replacedEntries
			<< ", preserved: " << stats.preservedEntries << "\n";
		return 0;
	}
}

int main(int argc, char **argv)
{
	if (argc == 2)
	{
		const std::string_view option = argv[1];
		if (option == "-h"sv || option == "--help"sv)
		{
			PrintUsage(argv[0]);
			return 0;
		}
	}

	try
	{
		if (argc < 2)
		{
			PrintUsage(argv[0]);
			return 2;
		}

		const std::string_view command = argv[1];
		int result = 2;
		if (command == "unpack"sv)
			result = RunUnpack(argc, argv);
		else if (command == "pack"sv)
			result = RunPack(argc, argv);
		else if (command == "patch"sv)
			result = RunPatch(argc, argv);

		if (result == 2)
			PrintUsage(argv[0]);

		return result;
	}
	catch (const std::exception &ex)
	{
		std::cerr << "Error: " << ex.what() << "\n";
		return 1;
	}
}
