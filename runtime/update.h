// The in-game update (update.c): the Menu tab of the settings menu checks GitHub for a newer release, downloads its
// Windows setup and starts it on this install folder. Windows only; any thread.
#pragma once
#include <stdint.h>

typedef enum {
    UPDATE_IDLE,         // no check yet in this run (or the last one failed)
    UPDATE_CHECKING,
    UPDATE_CURRENT,      // the running version is the latest release or newer
    UPDATE_NEWER,        // a newer release exists (version, size, page)
    UPDATE_DOWNLOADING,  // its setup is downloading (done of size bytes), then verified and unpacked
    UPDATE_READY,        // its setup is unpacked and verified: update_install can start it
} UpdateState;

typedef enum {
    UPDATE_OK,
    UPDATE_ERR_CHECK,     // the check got no answer from GitHub, or not one it could read
    UPDATE_ERR_VERSION,   // this build's version is not a release version: nothing to compare
    UPDATE_ERR_NO_SETUP,  // the newer release has no Windows setup with a SHA-256
    UPDATE_ERR_DOWNLOAD,  // the download broke off
    UPDATE_ERR_VERIFY,    // the download's size, SHA-256 or unpacked version did not match the release
    UPDATE_ERR_DISK,      // the setup could not be written or unpacked
    UPDATE_ERR_START,     // the setup did not start
} UpdateError;

typedef struct {
    UpdateState state;
    UpdateError error;    // the last job's failure (UPDATE_OK none); the log has the details
    char version[32];     // the latest release the last check found ("0.3.13"); "" before one did
    char page[256];       // its release page on GitHub; "" when the answer gave none
    uint64_t size, done;  // the setup zip's bytes and those downloaded so far
    int cancelling;       // update_cancel was called; the download stops at its next piece
    int setup_made;       // this folder was installed by the setup (totk-install.json): an update can run here
} UpdateStatus;

int update_supported(void);            // 0 on Linux: the menu shows no update rows
void update_menu_opened(void);         // the menu opened: the first call of a run checks while TOTK_UPDATE_CHECK is on
void update_status(UpdateStatus *out);
void update_check(void);               // a check now, unless a check or download runs
void update_download(void);            // downloads, verifies and unpacks the newer release's setup (UPDATE_NEWER)
void update_cancel(void);              // stops a download; its files go
// Starts the unpacked setup on this install folder (UPDATE_READY): it waits until this game has exited, updates the
// folder and starts the game again. 1 when it started: the caller then closes the game.
int update_install(void);
