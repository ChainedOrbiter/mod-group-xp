# ![logo](https://raw.githubusercontent.com/azerothcore/azerothcore.github.io/master/images/logo-github.png) AzerothCore
## mod-group-xp
### This is a module for [AzerothCore](http://www.azerothcore.org)

# Module info

- Name: Group XP Rebalancer
- Author: ChainedOrbiter
- Module:
    + Repository: [https://github.com/ChainedOrbiter/mod-group-xp](https://github.com/ChainedOrbiter/mod-group-xp)
- License: MIT License

# Module integration

- Includes configuration (.conf)?: Yes, copied by CMake
- Includes SQL patches?: No
- Core hooks used:
    + PlayerScript: OnLogin
    + PlayerScript: OnGiveXP
    + WorldScript: OnAfterConfigLoad

# Description
This module rebalances the amount of kill XP granted when in a party, primarily when in an open world setting (i.e. not in a dungeon or raid).

#### Features:
- Command handler for info & debugging: ``.groupxp``
- Designed for open-world leveling
- (Optional) Integrates with mod-open-world-party-scaling ([https://github.com/Stefan2102/mod-open-world-party-scaling](https://github.com/Stefan2102/mod-open-world-party-scaling)) to use as basis for multipliers. This allows harder fights to also give greater rewards, or vice versa.

### How to install
1. Simply place the module under the `modules` folder of your AzerothCore source folder.
2. Re-run cmake and build AzerothCore
3. that's all

## Configuration
The module uses direct decimal multipliers for party sizes 2, 3, 4, and 5. Raid groups are excluded.

```ini
GroupXP.Enable = 1
GroupXP.RequireSameMapAndZone = 1

# Party sizes
GroupXP.PartySize2.XPMultiplier = 1.10
GroupXP.PartySize3.XPMultiplier = 1.20
GroupXP.PartySize4.XPMultiplier = 1.30
GroupXP.PartySize5.XPMultiplier = 1.40
```

For example, with the settings above a party of 3 memebrs would receive 120% of regular XP. I.e. if a kill in the party would regularly award 100 XP it would instead give 130 XP.

### Open World Party Scaling integration
These settings allow the XP multipliers to be dynamically adjusted based on the difficulty modifiers from the mod-open-world-party-scaling module.

This integration is only active when:
1. GroupXP.OWPS.EnableIntegration = 1
2. The mod-open-world-party-scaling module is compiled and loaded

```ini
GroupXP.OWPS.EnableIntegration        = 1
GroupXP.OWPS.OverrideMultiplier       = 0
GroupXP.OWPS.DamageMultiplier         = 0.80
GroupXP.OWPS.HealingMultiplier        = 0.40
GroupXP.OWPS.IncomingDamageMultiplier = 0.80
```

How the calculation works:
- The base XP multiplier starts from GroupXP.PartySizeN.XPMultiplier
- OWPS modifiers contribute additively based on their difference from 1.0 (100%)
- Each contribution is weighted by the corresponding OWPS.*Multiplier setting

The OWPS multipliers are how heavily weighted the party scaling values are when taken into account for XP multipliers.

It's recommended to use the ``.groupxp`` command when balancing. The commands are quite  verbose when OWPS integration is enabled to give the full picture.

More details and examples can be found in the ``.conf`` file.

### Reloading config
The module is engineered to respect the ``.reload config``, so any value changes made in the ``.conf`` file will be updated directly. This can be verified with the chat commands.


## Credits
* Stefan2102 for making mod-open-world-party-scaling, inspiring this mod
* AzerothCore: [repository](https://github.com/azerothcore) - [website](http://azerothcore.org/) - [discord chat community](https://discord.gg/PaqQRkd)
