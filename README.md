# Broomy

A plugin for Crimson Desert that lets Kliff, Damiane and Oongka ride a flying
broom.

Pearl Abyss built a broom mount and left it out of the game. The archives
hold its model, a skeleton with two rider seats, eleven animations each for
the broom and for Kliff, and a `Broom_Ride` interaction, all unused. The
plugin wires them up as Broomy, a mount of its own in the saddle wedge of the
Character radial. The Wyvern and every other mount stay as they are.

Target: Crimson Desert 2.03.02 (exe 1.0.0.2976). Players should read
`mod/README.md`, which ships with the plugin and covers installing, settings
and the known limits.

## How it works

The plugin is the whole install. There's no pack group and no `0.papgt`
edit. Everything Broomy needs that the game doesn't ship comes from memory:

- Broomy's rows in ten static tables: stringinfo, characterinfo,
  mercenaryinfo, mercenarygroupinfo, reserveslot, vehicleinfo,
  characterappearanceindexinfo, interactioninfo, uimaptextureinfo and
  uifiltergroupinfo. The game reads its own file and the plugin swaps in its
  built copy.
- Charts made from the Wyvern's riding charts, with every animation path
  pointed at the broom's clips, the sounds and camera shakes cut, and the
  speeds taken from the ini.
- A pose modifier that turns the broom's body toward the camera in flight.
- Clips the game doesn't ship, built from its own: the broom held steady in
  level flight, the rider's push-off and leans, and Damiane's own riding
  clips, which nodes of hers added to Kliff's riding charts play. The
  riders' lean blends are laid out on Broomy's speeds.
- The horse's way of loading a bounty. Broomy's vehicle row gets the horse's cargo seat, its interaction list gets a copy of
  Horse_Carry_Dead that shows without focus, and the horse's Unload accepts
  Broomy. Every broom clip moves the cargo seat behind Kliff, and the
  bounty's lying clip is placed from it. Mounting with a bounty on the
  shoulder runs copies of the broom's mount and dismount that keep the
  carry, and an idle that holds him there in flight.
- A map icon drawn with the game's broom item icon.

A few seconds after a save loads, the plugin checks the roster. A save
without Broomy gets one by hiring a wild horse under Broomy's character row
on the game server's own thread, or after four seconds with no horse in
reach, another wild animal. Broomy's list is the third in the saddle
wedge, so the game's own call branch calls it. While Broomy is chosen, a
hook on the rider's chart checks stops a call while falling from bringing
Blackstar. Another has the game evaluate the rider's lean blend every frame,
so the lean eases with the speed, and another scales Broomy's speed by the
left stick. A call that finds Broomy marked dead clears it from the roster
entry, so the server spawns a fresh one. A call that finds it groggy (the
game's "fainted") clears it too, but only after the game's own groggy test
has said so, from a hook on that one call, because the test runs under a lock
a detour cannot take. The game refuses that press, and the next one brings a
fresh Broomy. A call that finds Broomy already out goes to the server twice,
which brings it back with the arrival the Ride prompt waits for. With Speeder
Bike loaded, each mount's riding chart keeps running after its first ride, so
Broomy's swap of Kliff's lower chart goes in only while Broomy's chart left its
ground idle more recently than the speeder's, which SpeederBike.asi exports.

The full account of how the mod was made, with the addresses, formats and
everything that was tried and dropped, is in
[docs/how-broomy-was-made.md](docs/how-broomy-was-made.md).

## Building

MSVC Build Tools 2022, with the CMake and Ninja they bundle.

    mod\build.bat

The plugin lands in `mod\dist\Broomy.asi`. It needs Ultimate ASI Loader
in the game's `bin64`.

## License

MIT. See LICENSE and THIRD_PARTY_NOTICES.md.
