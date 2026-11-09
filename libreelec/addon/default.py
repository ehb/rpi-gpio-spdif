# SPDX-License-Identifier: GPL-2.0-only
# The driver is loaded by system.d/driver.audio.rpi-gpio-spdif.service, which LibreELEC's
# service-addon-wrapper enables when the add-on is installed or enabled. This service only
# tells the user when that failed (most often: LibreELEC was updated and the module no longer
# matches the kernel); bin/rpi-gpio-spdif leaves the reason in STATUS.
import xbmc
import xbmcgui

STATUS = "/run/rpi-gpio-spdif.status"


def main():
    monitor = xbmc.Monitor()
    # the unit runs before Kodi, but a manual restart may still be under way
    if monitor.waitForAbort(10):
        return
    try:
        with open(STATUS, encoding="ascii", errors="replace") as f:
            reason = f.read().strip()
    except OSError:
        return
    if reason:
        xbmc.log("rpi-gpio-spdif: " + reason, xbmc.LOGWARNING)
        xbmcgui.Dialog().notification("S/PDIF out (GPIO21) not available", reason,
                                      xbmcgui.NOTIFICATION_WARNING, 20000)


main()
