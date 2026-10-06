# STM32L0538-DISCO-Demo

A working demonstration project for the **STMicroelectronics STM32L0538 Discovery board**, built with **STM32CubeIDE**.

This project exists primarily because documentation and example projects for this board are relatively limited, and there are **two versions of the STM32L0538 Discovery board** with differences that can make existing examples and documentation confusing to work from.

Rather than relying on incomplete or version-specific examples, this repository provides a practical, working reference for the board's hardware and peripherals.

## About the Board

The STM32L0538 Discovery board is based on the **STM32L0538** low-power STM32 microcontroller and includes an e-paper display and touch interface.

There are **two versions of the development board**. While they share the same general platform, differences between the revisions mean that documentation or example code written for one version may not always translate directly to the other.

Existing documentation and demonstration material for the board can also be difficult to piece together. This project is intended to fill some of that gap by providing a complete, buildable STM32CubeIDE project that can be used as a reference when working with the hardware.

## Precompiled Demo

If you have the **MB1143-L053C8T6-B03** version of the board, you can test the demonstration without installing STM32CubeIDE or compiling the project yourself.

A precompiled `.elf` binary is included with the project's releases.

The release binary can be programmed directly to the board using **STM32CubeProgrammer**.

### Programming the Demo

1. Connect the STM32L0538 Discovery board to your computer through the ST-LINK USB connection.
2. Install and open **STM32CubeProgrammer**.
3. Connect to the target using the ST-LINK interface.
4. Select the `.elf` file from the project release.
5. Program the device.
6. Reset the board and run the demonstration.

This provides a quick way to verify the board and demonstration without modifying or compiling any source code.

> **Important:** The precompiled release binary is intended for the **MB1143-L053C8T6-B03** board version. Make sure you have the correct board revision before programming the binary.

If you want to modify the firmware or develop your own application, use the STM32CubeIDE project instead.

## What This Project Demonstrates

The project currently focuses on:

- STM32L0538 initialization and peripheral configuration
- E-paper display operation
- Display graphics and text
- Touch input
- Integration of the board hardware into an STM32CubeIDE project

The **e-paper display is the primary focus** of the current demonstration.

## Touch Interface

The touch interface is currently **functional but needs improvement**.

Basic touch operation works, but the implementation is not yet as reliable or polished as it should be. Touch behavior, handling, and overall interaction are areas of active improvement.

For that reason, the touch portion of the project should currently be considered a **working demonstration rather than a finished implementation**.

### Known Touch Issues

- Touch response is not consistently reliable
- Touch handling needs refinement
- Coordinate/input handling could be improved
- The demonstration UI needs additional polish

Improving the touch implementation is one of the next priorities for the project.

## Requirements

### To run the precompiled demo

- **MB1143-L053C8T6-B03** Discovery board
- USB connection to the board
- [STM32CubeProgrammer](https://www.st.com/en/development-tools/stm32cubeprog.html)

### To build or modify the project

- STM32L0538 Discovery board
- USB connection to the board
- [STM32CubeIDE](https://www.st.com/en/development-tools/stm32cubeide.html)
- ST-LINK connection provided by the Discovery board

## Building the Project

Clone the repository:

```bash
git clone https://github.com/ZGoode/STM32L0538-DISCO-Demo.git
```

Open the project in **STM32CubeIDE** and import the existing project into your workspace.

The STM32Cube configuration is included in the project, so the peripheral configuration can also be inspected or modified through the `.ioc` file.

Build the project and program the board through the on-board **ST-LINK** interface.

## Project Structure

The project follows the standard STM32CubeIDE project layout, with the application and hardware configuration contained within the project directory.

The `.ioc` file contains the STM32Cube configuration used to configure the MCU and its peripherals.

Application and driver code is kept separate from the STM32-generated initialization code where practical.

## Current Status

**Overall:** Functional / Development

| Component | Status |
|---|---|
| STM32L0538 firmware | Working |
| STM32CubeIDE project | Working |
| E-paper display | Working |
| Display demonstration | Working |
| Touch interface | Functional, needs improvement |

The project is usable as a starting point for development on the STM32L0538 Discovery board, but the touch implementation is still being refined.

## Future Work

The immediate focus is improving the touch implementation and making the touch demonstration more reliable.

Other improvements will be made as additional hardware behavior is documented and tested across the different board versions.

## References

- [STM32L0538 Discovery BSP — STMicroelectronics](https://github.com/STMicroelectronics/32l0538discovery-bsp)
- [STM32CubeProgrammer — STMicroelectronics](https://www.st.com/en/development-tools/stm32cubeprog.html)
- [STM32CubeIDE — STMicroelectronics](https://www.st.com/en/development-tools/stm32cubeide.html)
- [STM32L0 Series — STMicroelectronics](https://www.st.com/en/microcontrollers-microprocessors/stm32l0-series.html)

## License

Shield: [![CC BY-NC-SA 4.0][cc-by-nc-sa-shield]][cc-by-nc-sa]

This work is licensed under a
[Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International License][cc-by-nc-sa].

[![CC BY-NC-SA 4.0][cc-by-nc-sa-image]][cc-by-nc-sa]

[cc-by-nc-sa]: http://creativecommons.org/licenses/by-nc-sa/4.0/
[cc-by-nc-sa-image]: https://licensebuttons.net/l/by-nc-sa/4.0/88x31.png
[cc-by-nc-sa-shield]: https://img.shields.io/badge/License-CC%20BY--NC--SA%204.0-lightgrey.svg
