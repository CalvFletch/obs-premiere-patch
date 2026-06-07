#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void mp_start(void);
void mp_stop(void);
void mp_on_recording_stopped(void);
void mp_on_obs_loaded(void);
void mp_open_fix_folder_dialog(void);
void mp_install_exiftool(void);

#ifdef __cplusplus
}
#endif
