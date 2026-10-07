#ifndef LIERO_WEAPSEL_HPP
#define LIERO_WEAPSEL_HPP

#include "game.hpp"
#include "menu/menu.hpp"
#include "gfx/renderer.hpp"

struct Game;

struct WeaponSelection
{
	WeaponSelection(Game& game);

	void draw(Renderer& renderer, GameState state, bool useSpectatorViewports);
	void drawNormalViewports(Renderer& renderer, GameState state);
	void drawSpectatorViewports(Renderer& renderer, GameState state);
	void drawControls(Renderer& renderer, std::size_t menuIdx, int x, int y, bool full);
	bool processFrame();
	void finalize();

	void focus();
	void unfocus();

	Game& game;

	int enabledWeaps;
	int fadeValue;
	std::vector<bool> isReady;
	std::vector<Menu> menus;
	bool cachedBackground, cachedSpectatorBackground;
	bool focused;

	// One player mode: a single full width screen for the human; the computer picks unseen
	bool onePlayer;
	// Network games: only show controls for this worm, using this Mac's keys (-1 = local game)
	int netLocalWorm;
};

//void selectWeapons(Game& game);

#endif // LIERO_WEAPSEL_HPP
