Bloodborne-Linux runs your own dump of Bloodborne (CUSA03173 with update 1.09) on Linux.

Start bb-launcher. It finds a dump in the game/ folder next to it, or you drag the dump's
folder onto its window. Everything the game writes goes to user/ next to the launcher.

Controller
  The launcher and the game work with a controller alone. In the launcher, A (Cross) chooses
  and B (Circle) goes back. To leave the game, hold Back and Start (Share and Options, View and
  Menu on a Steam Deck) together for two seconds: you are back in the launcher.

Steam and Steam Deck (Game Mode)
  1. In Steam's desktop mode, choose Games > Add a Non-Steam Game to My Library..., then Browse
     and pick bb-launcher in this folder (or the .AppImage file). Leave the launch options empty.
  2. Switch to Game Mode and start it from your library. The launcher fills the screen and
     Steam's controller drives it. Play starts the game fullscreen at the screen's size.
  3. Hold Back and Start to leave the game, then choose Quit in the launcher to return to Steam.

Outside Steam, gamescope works the same way: gamescope -f -- ./bb-launcher
