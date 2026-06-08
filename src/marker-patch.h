#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Lifecycle */
void mp_start(void);
void mp_stop(void);

/* OBS event handlers */
void mp_on_recording_started(void);
void mp_on_recording_stopped(void);
void mp_on_obs_loaded(void);

/* Menu setup (called from obs_module_load, implemented in menu.cpp) */
void mp_setup_menu(void);

/* Auto-behaviour toggles (0 = off, 1 = on) */
int  mp_get_auto_markers(void);
int  mp_get_auto_trim(void);
void mp_set_auto_markers(int on);
void mp_set_auto_trim(int on);

/* Manual fix actions (each shows a folder or file picker dialog) */
void mp_fix_folder_markers(void);
void mp_fix_file_markers(void);
void mp_fix_folder_trim(void);
void mp_fix_file_trim(void);

#ifdef __cplusplus
}
#endif
