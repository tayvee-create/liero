#ifndef UUID_DC1D9513CDD34960AB8A648004DA149D
#define UUID_DC1D9513CDD34960AB8A648004DA149D

#include <SDL.h>

#include "gfx.hpp"
#include "sfx.hpp"
#include "sys.hpp"
#include "game.hpp"
#include "viewport.hpp"
#include "worm.hpp"
#include "reader.hpp"
#include "filesystem.hpp"
#include "text.hpp"
#include "keys.hpp"
#include "constants.hpp"
#include "math.hpp"
#include "console.hpp"
#include "controller/netController.hpp"
#include <gvl/support/platform.hpp>

#include <algorithm>
#include <ctime>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <gvl/math/cmwc.hpp>

#if __APPLE__
#define gameEntry SDL_main
#endif

void benchAll();

int gameEntry(int argc, char* argv[])
try
{
	// TODO: Better PRNG seeding
	gfx.rand.seed(Uint32(std::time(0)));

#if 0
	benchAll();
	return 0;
#elif 0
	std::string const& path = "D:\\cpp\\liero\\tc";
	FsNode node(path);

	for (DirectoryListing dir = node.iter(); dir; ++dir)
	{
		auto d = *dir;

		try
		{
			auto zipNode = node / d;

			bool found = false;

			for (DirectoryListing zipDir = zipNode.iter(); zipDir; ++zipDir)
			{
				auto e = *zipDir;
				auto upper = e;
				toUpperCase(upper);

				if (upper.find(".EXE") != std::string::npos && upper != "LEVEDIT.EXE")
				{
					found = true;
					Common common(zipNode, e);

					printf("N: %s\n", common.guessName().c_str());
					//printf("N: %s ||| %s\n", common.S[SCopyright2].c_str(), common.S[SCopyright].c_str() + 4);
					break;
				}
			}

			if (!found)
				printf("** No exe found for %s\n", d.c_str());
		}
		catch (std::runtime_error& e)
		{
			//printf("Failed to load: %s\n", d.c_str());
			//printf("%s\n", e.what());
		}
	}

	return 0;
#endif

	bool tcSet = false;

	std::string tcName;
	std::string configPath; // Default to current dir

	for(int i = 1; i < argc; ++i)
	{
		if(argv[i][0] == '-')
		{
			switch(argv[i][1])
			{
			case '-':
				if (std::strcmp(argv[i] + 2, "config-root") == 0 && i + 1 < argc)
				{
					++i;
					configPath = argv[i];
				}
				else if (std::strcmp(argv[i] + 2, "net-host") == 0)
					netAutoStart.mode = 1;
				else if (std::strcmp(argv[i] + 2, "net-join") == 0 && i + 1 < argc)
				{
					netAutoStart.mode = 2;
					netAutoStart.ip = argv[++i];
				}
				else if (std::strcmp(argv[i] + 2, "two-player") == 0)
					netAutoStart.mode = 5;
				else if (std::strcmp(argv[i] + 2, "one-player") == 0 && i + 1 < argc)
					netAutoStart.mode = std::strcmp(argv[++i], "hard") == 0 ? 4 : 3;
				else if (std::strcmp(argv[i] + 2, "net-bot") == 0)
					netAutoStart.bot = true;
				break;
			}
		}
		else
		{
			tcName = argv[i];
			tcSet = true;
		}
	}

#if __APPLE__
	// Inside a .app the working directory is the read-only Resources folder.
	// Keep settings, profiles and replays in Application Support instead, seeded
	// with the bundled data (refreshed whenever the app ships newer files).
	if (configPath.empty() && std::getenv("HOME"))
	{
		namespace fs = std::filesystem;
		fs::path support = fs::path(std::getenv("HOME")) / "Library/Application Support/OpenLiero";
		std::error_code ec;
		fs::create_directories(support, ec);
		for (char const* item : {"TC", "icon.png", "spectator_icon.png"})
		{
			if (fs::exists(item, ec))
				fs::copy(item, support / item, fs::copy_options::recursive | fs::copy_options::update_existing, ec);
		}
		if (!ec)
			configPath = support.string() + "/";
	}
#endif

	SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER);

	initKeys();

	precomputeTables();

	gfx.loadMenus();

	gfx.init();
	gfx.setConfigPath(configPath);

	FsNode configNode(gfx.getConfigNode());

	if (!gfx.loadSettings(configNode / "liero.cfg"))
	{
		if (!gfx.loadSettingsLegacy(configNode / "LIERO.DAT"))
		{
			gfx.settings.reset(new Settings);
			gfx.saveSettings(configNode / "liero.cfg");
		}
	}

	if (tcSet)
		gfx.settings->tc = tcName;

	{
		// Move the right player off the old PC defaults (Right Ctrl/Alt/Shift),
		// unless they have been changed by hand
		uint32_t const oldKeys[7] = {0xA0, 0xA8, 0xA3, 0xA5, 0x75, 0x90, 0x36};
		uint32_t const newKeys[7] = {0xA0, 0xA8, 0xA3, 0xA5, 0x34, 0x33, 0x35};
		WormSettings& right = *gfx.settings->wormSettings[1];
		if (std::equal(oldKeys, oldKeys + 7, right.controlsEx))
		{
			std::copy(newKeys, newKeys + 7, right.controls);
			std::copy(newKeys, newKeys + 7, right.controlsEx);
		}
	}

	// TC loading
	FsNode lieroRoot(configNode / "TC" / gfx.settings->tc);
	gvl::shared_ptr<Common> common(new Common());
	common->load(std::move(lieroRoot));
	gfx.common = common;
	gfx.playRenderer.loadPalette(*common); // This gets the palette from common

	gfx.setVideoMode();
	sfx.init();

	gfx.mainLoop();

	gfx.restorePlayerControllers();
	gfx.settings->save(configNode / "liero.cfg", gfx.rand);

	sfx.deinit();
	SDL_Quit();

	//gvl::present_profile(std::cout);

	return 0;
}
catch(std::exception&)
{
	SDL_Quit();
	throw;
}

#endif // UUID_DC1D9513CDD34960AB8A648004DA149D
