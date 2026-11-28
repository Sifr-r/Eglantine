// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub enum LibraryLayout {
    Existing,
    List,
    Grid,
    Shelf,
    SplitPreview,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ConsoleFamily {
    Home,
    Handheld,
}

use ConsoleFamily::{Handheld, Home};
use LibraryLayout::{Existing, Grid, List, Shelf, SplitPreview};

macro_rules! console_catalog {
    ($($variant:ident => ($id:literal, $name:literal, $family:ident, $layout:ident, $initial:literal)),+ $(,)?) => {
        #[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
        pub enum ConsoleId { $($variant),+ }

        pub const CONSOLES: &[ConsoleId] = &[$(ConsoleId::$variant),+];

        impl ConsoleId {
            pub const fn as_str(self) -> &'static str {
                match self { $(Self::$variant => $id),+ }
            }

            pub fn parse(id: &str) -> Option<Self> {
                match id { $($id => Some(Self::$variant),)+ _ => None }
            }

            pub const fn name(self) -> &'static str {
                match self { $(Self::$variant => $name),+ }
            }

            pub const fn family(self) -> ConsoleFamily {
                match self { $(Self::$variant => $family),+ }
            }

            pub const fn layout(self) -> LibraryLayout {
                match self { $(Self::$variant => $layout),+ }
            }

            /// Identifies first-release platforms. Hosts separately check runtime availability.
            pub const fn supported(self) -> bool {
                match self { $(Self::$variant => $initial),+ }
            }
        }
    };
}

console_catalog! {
    Switch => ("switch", "Nintendo Switch", Home, Existing, true),
    Switch2 => ("switch-2", "Nintendo Switch 2", Home, Existing, true),
    GameBoy => ("game-boy", "Game Boy", Handheld, List, true),
    GameBoyColor => ("game-boy-color", "Game Boy Color", Handheld, Grid, true),
    GameBoyAdvance => ("game-boy-advance", "Game Boy Advance", Handheld, SplitPreview, true),
    Nes => ("nes", "NES / Famicom", Home, List, false),
    FamicomDiskSystem => ("famicom-disk-system", "Famicom Disk System", Home, List, false),
    Snes => ("snes", "SNES / Super Famicom", Home, Grid, false),
    Satellaview => ("satellaview", "Satellaview", Home, Grid, false),
    Nintendo64 => ("nintendo-64", "Nintendo 64", Home, Shelf, false),
    Nintendo64Dd => ("nintendo-64dd", "Nintendo 64DD", Home, Shelf, false),
    IQuePlayer => ("ique-player", "iQue Player", Home, Shelf, false),
    Ds => ("ds", "Nintendo DS", Handheld, Grid, false),
    Dsi => ("dsi", "Nintendo DSi", Handheld, Grid, false),
    Nintendo3Ds => ("nintendo-3ds", "Nintendo 3DS", Handheld, Grid, false),
    GameCube => ("gamecube", "GameCube", Home, Grid, false),
    Wii => ("wii", "Wii", Home, Grid, false),
    WiiU => ("wii-u", "Wii U", Home, SplitPreview, false),
    VirtualBoy => ("virtual-boy", "Virtual Boy", Home, SplitPreview, false),
    PokemonMini => ("pokemon-mini", "Pokémon mini", Handheld, List, false),
    GameAndWatch => ("game-and-watch", "Game & Watch", Handheld, List, false),
    ColorTvGame => ("color-tv-game", "Color TV-Game", Home, List, false),
}

#[cfg(test)]
mod tests {
    use super::{ConsoleId, LibraryLayout, CONSOLES};

    #[test]
    fn catalog_ids_round_trip_and_are_unique() {
        let mut ids = std::collections::BTreeSet::new();
        for &console in CONSOLES {
            assert!(ids.insert(console.as_str()));
            assert_eq!(ConsoleId::parse(console.as_str()), Some(console));
            assert!(!console.name().is_empty());
        }
        assert_eq!(ConsoleId::parse("other"), None);
    }

    #[test]
    fn initial_catalog_assigns_distinct_portable_layouts() {
        assert_eq!(ConsoleId::GameBoy.layout(), LibraryLayout::List);
        assert_eq!(ConsoleId::GameBoyColor.layout(), LibraryLayout::Grid);
        assert_eq!(ConsoleId::GameBoyAdvance.layout(), LibraryLayout::SplitPreview);
        assert_eq!(ConsoleId::Switch2.layout(), LibraryLayout::Existing);
        assert!(!ConsoleId::Ds.supported());
    }
}
