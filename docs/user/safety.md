# Safety checklist

Complete this physical checklist before any real-camera `ost`, Python, viewer, ScopeFoundry or hardware-check command. It follows the public controller manuals and ordinary safe-operation practice. openst133 cannot verify protective grounding, set the controller's physical shutter drive or make controller power-off safe while liquid nitrogen remains.

## Before power on

- [ ] Keep the controller and computer off while connecting or disconnecting the USB cable. Keep the controller off while changing detector-controller or remote-shutter cabling, and secure both ends of the detector-controller cable before power on. (ST-133, pages 22-23; MicroMAX, pages 14 and 39-40)
- [ ] Verify that the power cord's ground prong connects to a properly grounded wall outlet. Never disconnect protective earth, and do not use damaged equipment until authorized personnel verify its safety. (ST-133, page 12; MicroMAX, pages 13-14)
- [ ] Check the rear-panel voltage selection and fuse label against the local supply and the manual. Use a compatible power cord and preserve the required plug polarity. (ST-133, page 13)
- [ ] With the controller off, check that the rear-panel Shutter Setting selector matches the installed shutter in the manual's table. The factory sets it for an internal shutter when present; a wrong setting can damage the shutter. This is an installation check, not a control exposed by openst133. (ST-133, page 16; MicroMAX, page 39)
- [ ] Keep the USB cable length within the manual's 5 m (16.4 ft) maximum. (ST-133, page 108; MicroMAX, page 155)
- [ ] Keep an LN-cooled detector connected to an operating controller whenever liquid nitrogen remains in the dewar. Closing the camera in software does not replace this power requirement. (ST-133, page 38)

## Open the camera

1. Complete the preflight checks and power on the controller.
2. Wait for USB enumeration and run `ost list`; do not open the camera until the ST-133 interface appears.
3. Open the camera with the chosen openst133 command or application.

Controller power and USB enumeration must precede opening the device. (MicroMAX, page 44)

## Shut down or change power

1. Stop acquisition and close every open camera device or application before controller power off. In Python, leave the camera context manager or call `close()`.
2. If liquid nitrogen remains in the dewar, do not turn the controller off: leave the detector connected to the operating controller. (ST-133, page 38)
3. After any controller or host power change, leave openst133 closed during the change, power on the controller, wait for `ost list` to show the interface, then reopen the device. (MicroMAX, page 44)
4. Before changing cabling, return to the unpowered-cabling checks above. The LN power requirement still applies.

## Manual references

- **ST-133**: Roper Scientific, *ST-133 Controller Manual*, 4411-0087 Version 3.B, May 14, 2004: grounding and power on pages 12-13, shutter drive on page 16, cabling on pages 22-23, LN cooling on page 38 and USB length on page 108.
- **MicroMAX**: Princeton Instruments, *MicroMAX System User Manual*, 4411-0039-CE Version 6.C, April 18, 2006: precautions on pages 13-14, shutter and cable setup on pages 39-40, USB power order on page 44 and USB length on page 155.
