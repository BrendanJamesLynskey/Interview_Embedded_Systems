# Embedded Systems Interview Preparation

Badge: Embedded Systems

A comprehensive study repository for embedded systems interview preparation, covering bare-metal C programming, real-time operating systems, communication protocols, debugging techniques, and system design principles.

## Table of Contents

- [01 Bare Metal Fundamentals](#01-bare-metal-fundamentals)
- [02 RTOS](#02-rtos)
- [03 Communication Protocols](#03-communication-protocols)
- [04 Debugging and Testing](#04-debugging-and-testing)
- [05 System Design](#05-system-design)
- [06 Quizzes](#06-quizzes)
- [How to Use](#how-to-use)
- [Contributing](#contributing)
- [Related Repositories](#related-repositories)

## 01 Bare Metal Fundamentals

Understanding the lowest levels of embedded systems: startup sequences, memory management, interrupt handling, and direct hardware access.

- [Startup and Boot Sequence](01_bare_metal_fundamentals/startup_and_boot_sequence.md)
- [Linker Scripts and Memory Layout](01_bare_metal_fundamentals/linker_scripts_and_memory_layout.md)
- [Interrupt Handling](01_bare_metal_fundamentals/interrupt_handling.md)
- [Peripheral Register Access](01_bare_metal_fundamentals/peripheral_register_access.md)
- [Volatile and Memory Barriers](01_bare_metal_fundamentals/volatile_and_memory_barriers.md)
- [Coding Challenges](01_bare_metal_fundamentals/coding_challenges/)

## 02 RTOS

Real-time operating systems: task scheduling, synchronization primitives, priority inversion, and FreeRTOS-specific considerations.

- [RTOS Fundamentals](02_rtos/rtos_fundamentals.md)
- [Tasks and Scheduling](02_rtos/tasks_and_scheduling.md)
- [Semaphores, Mutexes, and Queues](02_rtos/semaphores_mutexes_queues.md)
- [Priority Inversion](02_rtos/priority_inversion.md)
- [FreeRTOS Specifics](02_rtos/freertos_specifics.md)
- [Coding Challenges](02_rtos/coding_challenges/)

## 03 Communication Protocols

Low-level and high-level communication: UART, SPI, I2C, CAN, Ethernet/TCP-IP, and USB device protocols.

- [UART, SPI, I2C Deep Dive](03_communication_protocols/uart_spi_i2c_deep_dive.md)
- [CAN Bus](03_communication_protocols/can_bus.md)
- [Ethernet and TCP/IP](03_communication_protocols/ethernet_and_tcpip.md)
- [USB Device](03_communication_protocols/usb_device.md)
- [Coding Challenges](03_communication_protocols/coding_challenges/)

## 04 Debugging and Testing

Tools and techniques for embedded systems debugging and validation: JTAG/SWD, oscilloscopes, memory corruption detection, and unit testing.

- [JTAG and SWD](04_debugging_and_testing/jtag_and_swd.md)
- [Logic Analyser and Oscilloscope](04_debugging_and_testing/logic_analyser_and_scope.md)
- [Memory Corruption Debugging](04_debugging_and_testing/memory_corruption_debugging.md)
- [Unit Testing in Embedded Systems](04_debugging_and_testing/unit_testing_embedded.md)
- [Worked Problems](04_debugging_and_testing/worked_problems/)

## 05 System Design

High-level system architecture: bootloaders, OTA updates, power management, and safety-critical system design.

- [Bootloader Design](05_system_design/bootloader_design.md)
- [OTA Firmware Update](05_system_design/ota_firmware_update.md)
- [Low Power Design](05_system_design/low_power_design.md)
- [Safety Critical (IEC 61508)](05_system_design/safety_critical_iec61508.md)
- [Worked Problems](05_system_design/worked_problems/)

## 06 Quizzes

Practice quizzes organized by topic to test knowledge and prepare for technical interviews.

- [Quiz: Bare Metal](06_quizzes/quiz_bare_metal.md)
- [Quiz: RTOS](06_quizzes/quiz_rtos.md)
- [Quiz: Protocols](06_quizzes/quiz_protocols.md)
- [Quiz: Debugging](06_quizzes/quiz_debugging.md)
- [Quiz: System Design](06_quizzes/quiz_system_design.md)

## How to Use

This repository is structured as a progressive learning resource for embedded systems interview preparation.

1. **Start with Fundamentals**: Begin with bare-metal concepts in section 01 to build a foundation of hardware understanding.

2. **Progress to Higher Abstractions**: Move through sections 02-03 to understand how operating systems and communication layers build on bare-metal principles.

3. **Apply Knowledge**: Work through the coding challenges in each section to reinforce conceptual understanding with practical implementation.

4. **Study Worked Problems**: Review the worked problems in sections 04-05 to see how debugging and design techniques apply in realistic scenarios.

5. **Self-Assess**: Use the quizzes in section 06 to identify knowledge gaps and track preparation progress.

Each section includes both conceptual material (markdown files) and practical exercises (C code challenges and worked problems).

## Contributing

Contributions are welcome. To contribute:

1. Create a new branch for your changes.
2. Follow the existing structure and naming conventions.
3. Ensure markdown files are well-formatted and C code is properly commented.
4. Submit a pull request with a clear description of your additions or improvements.

## Related Repositories

- [Interview_C](https://github.com/BrendanJamesLynskey/Interview_C) - C language fundamentals and advanced topics.
- [RISCV_RV32I_SingleCycle](https://github.com/BrendanJamesLynskey/RISCV_RV32I_SingleCycle) - Single-cycle RISC-V processor implementation.
- [RISCV_SoC](https://github.com/BrendanJamesLynskey/RISCV_SoC) - Complete System-on-Chip design and implementation.
