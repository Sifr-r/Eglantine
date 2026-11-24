// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Direction {
    Left,
    Right,
    Up,
    Down,
}

pub fn move_focus<T: Copy + Eq>(rows: &[Vec<T>], current: T, direction: Direction) -> Option<T> {
    let Some((row, column)) = rows.iter().enumerate().find_map(|(row, items)| {
        items.iter().position(|item| *item == current).map(|column| (row, column))
    }) else {
        return rows.iter().find_map(|items| items.first().copied());
    };
    match direction {
        Direction::Left => rows[row].get(column.saturating_sub(1)).copied(),
        Direction::Right => rows[row].get(column + 1).copied().or(Some(current)),
        Direction::Up | Direction::Down => {
            let adjacent = match direction {
                Direction::Up => rows[..row].iter().rev().find(|items| !items.is_empty()),
                Direction::Down => rows[row + 1..].iter().find(|items| !items.is_empty()),
                _ => unreachable!(),
            };
            adjacent.map(|items| items[column.min(items.len() - 1)]).or(Some(current))
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn moving_between_rows_clamps_to_the_available_column() {
        let rows = vec![vec![0, 1, 2, 3], vec![4, 5]];
        assert_eq!(move_focus(&rows, 3, Direction::Down), Some(5));
        assert_eq!(move_focus(&rows, 5, Direction::Up), Some(1));
    }

    #[test]
    fn skips_empty_rows_and_keeps_focus_at_an_edge() {
        let rows = vec![vec![0], Vec::new(), vec![1, 2]];
        assert_eq!(move_focus(&rows, 0, Direction::Down), Some(1));
        assert_eq!(move_focus(&rows, 2, Direction::Right), Some(2));
        assert_eq!(move_focus(&rows, 0, Direction::Up), Some(0));
    }

    #[test]
    fn selects_the_first_available_target_when_the_current_one_disappears() {
        let rows = vec![Vec::new(), vec![1, 2]];
        assert_eq!(move_focus(&rows, 9, Direction::Right), Some(1));
        assert_eq!(move_focus::<i32>(&[], 9, Direction::Right), None);
    }
}
