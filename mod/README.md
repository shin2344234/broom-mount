# Broomy 1.0.7

For Crimson Desert's DLC build, exe 1.0.0.3392 (Steam build 26308950).

Pearl Abyss built a flying broom mount and never switched it on. The model,
its skeleton and its animations are all in the game's archives. This plugin
turns it into Broomy, a mount of its own in the saddle wedge of the
Character radial. Kliff, Damiane and Oongka can all ride it. No other mount
is changed.

## Installing

Copy `Broomy.asi` into the game's `bin64` folder, next to
`CrimsonDesert.exe`, with the game closed. You need an ASI loader there
already. Ultimate ASI Loader installed as `winmm.dll` is what most Crimson
Desert setups use, and the Definitive Mod Manager installs one for you.

No game file is modified. The first time it runs, the plugin writes
`Broomy.ini` beside itself with every setting at its default. An ini you
already have is left alone.

## Getting Broomy

Load a save. A few seconds later the plugin looks for Broomy among your
mounts, and if it is missing, a wild horse the game has loaded joins your
roster as Broomy. With no wild horse nearby, another wild animal joins
instead after a few more seconds, never a person. It happens once per save. A save that has Broomy already is
left alone.

Broomy is in the saddle wedge on the Character radial, after the horses and
the Wyvern. Choose it there, and the call key calls Broomy from then on, the
way it calls any mount you choose. Kliff, Damiane and Oongka share the one
Broomy. A save from an earlier version keeps its Broomy, which moves to the
saddle wedge.

## Riding

Mount from either side. On the ground Broomy hovers and moves about, and at
rest it floats gently up and down. Jump to take off. The broom dips and the
rider pushes off the ground with both feet. In the air the nose turns to
follow the camera, the run key boosts, and letting go of the stick holds
Broomy in place. The flying controls are the Wyvern's.

The faster Broomy flies, the further the rider leans over the handle, from
mostly upright at a slow push to down against it at full boost. In level
flight the broom holds still, so the hands stay on the grip.

After a faint the game lets you call Broomy again within a second, where it
used to make you wait five minutes. If the game still says "Cannot summon
because it's fainted" or "because it's dead", that press is refused, and the
next one brings a fresh Broomy.

With Speeder Bike installed and both mounts out, Kliff's riding pose follows
the one he's sitting on, so getting off the speeder and onto Broomy no longer
leaves him in the speeder's pose, or the other way round.

On a controller the speed follows how far the left stick is pushed. The
boost and the keyboard always move at full speed.

## Bounties

Broomy takes a bounty the way a horse does. With a bound bounty on your
shoulder, walk up to Broomy and press Load. You don't need focus for it. He
is laid across the broom behind the seat and stays there while you fly. To
take him back, get off and press Unload beside the broom.

You can also skip the load and climb on with him on your shoulder, as on a
horse. He rides there through the flight and is still on your shoulder when
you get off.

## Settings

All of them live in `Broomy.ini` and are read when the game starts.

    GiveBroomy=1       give Broomy to a save that does not have it
    GroundSpeed=250    speed on the ground, percent of the Wyvern's
    FlightSpeed=250    speed in the air, percent of the Wyvern's
    BoostSpeed=200     the boost, percent of Broomy's flight speed
    ClimbSpeed=200     climbing and diving, percent of the Wyvern's
    SlowestPush=15     speed at the lightest stick push, percent of full

At the defaults Broomy cruises at 62.5 m/s and boosts at 125. Set
`SlowestPush=100` for one speed whatever the push.

INI Master (https://www.nexusmods.com/crimsondesert/mods/3578) can edit these
with a label and range for each.

## Uninstalling

Back up your saves, then delete the `Broomy` files from `bin64`. Saves
with Broomy loaded without the plugin in testing, but one player's save
stopped at the loading screen once the plugin was gone, and the cause is
not known yet. Choose a horse in the saddle wedge before your last save
with the plugin, so the save does not name Broomy's list as the chosen
one. If you save without the plugin, that save loses Broomy, and putting
the plugin back gives you a new one.

## Known limits

- With Broomy chosen in the saddle wedge, the call key does nothing while
  you fall, so choose Blackstar's wedge first if you want Blackstar to catch
  you.
- Some places refuse every summon, the Abyss Nexus among them. That is the
  game's own rule. Flight Freedom 1.1.8 lifts it with its BlockedSummon
  setting.
