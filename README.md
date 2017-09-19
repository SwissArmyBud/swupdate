# Overview
[SWupdate](https://sbabic.github.io/swupdate/) is a program developed by [DENX](http://denx.de/en/News/WebHome) to assist with the orderly upgrade of embedded Linux systems. SWupdate can handle full image-to-partition functions, as well as handling individual files, running scripts, setting boot parameters, and running its own upgrade server if needed.

# Details
## Current Functionality
In addition to the base functionality of SWupdate, this fork has the following features implimented:
  - Individual files can be deleted
    - "file" must be REMOVE.KEY (and included in sw-description)
	- "path" is the path to delete, either file or directory
	- "type" must be "remove"
	- "device" will mount a device then delete the path on that device
  
## Future Functionality
The development of this fork will focus on the following issues, moving forward:
  - Individual files do not need to be put in existing directories
  - Failed updates integrate with UBoot to better recover (see next)
  - True power-off safety (the current system is not, despite claims)
  - Move and copy handlers

## Tooling
In order to ease development and application of updates, several small tools are available. They are called [SWupdated](https://github.com/SwissArmyBud/SWupdated) (for SWupdate Daemon) and provide an update builder conforming to the SWupdate specification as well as a daemon capable of automatically finding and running updates on inserted media like USB drives.

# --- ORIGINAL DOCUMENTATION BELOW ---
<p align ="center"><img src=SWUpdate.png width=400 height=400 /></p>

SWUpdate - Software Update for Embedded Systems
===============================================

[![Build Status](https://travis-ci.org/sbabic/swupdate.svg?branch=master)](https://travis-ci.org/sbabic/swupdate)

SWUpdate is a Linux Update agent with the goal to
provide an efficient and safe way to update
an embedded system. SWUpdate supports local and remote
updates, multiple update strategies and it can
be well integrated in the [Yocto](https://www.yoctoproject.org) build system by adding
the [meta-swupdate](https://layers.openembedded.org/layerindex/branch/master/layer/meta-swupdate/) layer.

Examples using this layer with evaluation boards (Beaglebone, RPI3) are provided in 
[meta-swupdate-boards](https://layers.openembedded.org/layerindex/branch/master/layer/meta-swupdate-boards/) layer.

It supports the common media on embedded devices 
such as as NOR / NAND flashes, UBI volumes, SD / eMMC, and can
be easy extended to introduce project specific update
procedures.

Pre- and postinstall scripts are supported, and a LUA
interpreter helps to customize the update process.

An update package is described by the sw-description file,
using the libconfig syntax or JSON. It is even possible to
use Lua with a custom syntax.

Here a short list of the main features:

- Install on embedded media (eMMC, SD, Raw NAND, NOR and SPI-NOR flashes)
- Allow delivery single image for multiple devices
- Multiple interfaces for getting software
    - local storage
    - integrated web server
    - integrated REST client connector to [hawkBit](https://projects.eclipse.org/projects/iot.hawkbit)
    - remote server download
- Software delivered as images, gzipped tarball, etc.
- Allow custom handlers for installing FPGA firmware, microcontroller firmware via custom protocols.
- Power-Off safe
- Hardware / Software compatibility.

This software is licensed under GPL Version 2.0+

Please check inside doc directory for documentation or
the online documentation (generated from doc/) at:
http://sbabic.github.io/swupdate


Submitting patches
------------------

Contributions are welcome !  You can submit your patches (or post questions
regarding the project) to the swupdate Mailing List:

	swupdate@googlegroups.com

When creating patches, please use something like:

    git format-patch -s <revision range>

Please use 'git send-email' to send the generated patches to the ML
to bypass changes from your mailer.

Patches are tracked by patchwork (see http://jk.ozlabs.org/projects/patchwork/).
You can see the status of your patches at
http://patchwork.ozlabs.org/project/swupdate/list.
