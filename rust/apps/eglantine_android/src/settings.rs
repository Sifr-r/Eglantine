// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

const RESOLUTION_LABELS: [&str; 5] = ["0.5x", "0.75x", "1x", "1.5x", "2x"];
const GPU_ACCURACY_LABELS: [&str; 2] = ["Low", "High"];

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SettingsSnapshot {
    pub use_docked_mode: bool,
    pub async_gpu: bool,
    pub disk_shader_cache: bool,
    pub use_vsync: bool,
    pub resolution_index: i32,
    pub gpu_accuracy: i32,
}

impl Default for SettingsSnapshot {
    fn default() -> Self {
        Self {
            use_docked_mode: false,
            async_gpu: true,
            disk_shader_cache: true,
            use_vsync: true,
            resolution_index: 2,
            gpu_accuracy: 0,
        }
    }
}

impl SettingsSnapshot {
    pub(super) fn normalized(mut self) -> Self {
        self.resolution_index = self.resolution_index.clamp(0, (RESOLUTION_LABELS.len() - 1) as i32);
        self.gpu_accuracy = self.gpu_accuracy.clamp(0, (GPU_ACCURACY_LABELS.len() - 1) as i32);
        self
    }

    pub(super) fn toggle_docked(&mut self) {
        self.use_docked_mode = !self.use_docked_mode;
    }

    pub(super) fn toggle_async_gpu(&mut self) {
        self.async_gpu = !self.async_gpu;
    }

    pub(super) fn toggle_shader_cache(&mut self) {
        self.disk_shader_cache = !self.disk_shader_cache;
    }

    pub(super) fn toggle_vsync(&mut self) {
        self.use_vsync = !self.use_vsync;
    }

    pub(super) fn cycle_resolution(&mut self) {
        let len = RESOLUTION_LABELS.len() as i32;
        self.resolution_index = (self.resolution_index.clamp(0, len - 1) + 1) % len;
    }

    pub(super) fn cycle_gpu_accuracy(&mut self) {
        let len = GPU_ACCURACY_LABELS.len() as i32;
        self.gpu_accuracy = (self.gpu_accuracy.clamp(0, len - 1) + 1) % len;
    }

    pub(super) fn resolution_label(self) -> &'static str {
        RESOLUTION_LABELS[self.resolution_index.clamp(0, (RESOLUTION_LABELS.len() - 1) as i32) as usize]
    }

    pub(super) fn gpu_accuracy_label(self) -> &'static str {
        GPU_ACCURACY_LABELS[self.gpu_accuracy.clamp(0, (GPU_ACCURACY_LABELS.len() - 1) as i32) as usize]
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn normalized_clamps_out_of_range_indices() {
        let clamped = SettingsSnapshot {
            resolution_index: 99,
            gpu_accuracy: -3,
            ..SettingsSnapshot::default()
        }
        .normalized();
        assert_eq!(clamped.resolution_index, 4);
        assert_eq!(clamped.gpu_accuracy, 0);
        assert_eq!(clamped.resolution_label(), "2x");
        assert_eq!(clamped.gpu_accuracy_label(), "Low");
    }

    #[test]
    fn toggles_and_cycles_wrap_cleanly() {
        let mut settings = SettingsSnapshot::default();
        settings.toggle_docked();
        settings.toggle_async_gpu();
        settings.toggle_shader_cache();
        settings.toggle_vsync();
        assert!(settings.use_docked_mode);
        assert!(!settings.async_gpu);
        assert!(!settings.disk_shader_cache);
        assert!(!settings.use_vsync);

        assert_eq!(settings.resolution_label(), "1x");
        settings.cycle_resolution();
        assert_eq!(settings.resolution_label(), "1.5x");
        settings.cycle_resolution();
        assert_eq!(settings.resolution_label(), "2x");
        settings.cycle_resolution();
        assert_eq!(settings.resolution_label(), "0.5x");

        assert_eq!(settings.gpu_accuracy_label(), "Low");
        settings.cycle_gpu_accuracy();
        assert_eq!(settings.gpu_accuracy_label(), "High");
        settings.cycle_gpu_accuracy();
        assert_eq!(settings.gpu_accuracy_label(), "Low");
    }
}
