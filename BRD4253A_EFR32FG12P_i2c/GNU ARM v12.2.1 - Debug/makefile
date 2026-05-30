################################################################################
# Automatically-generated file. Do not edit!
################################################################################

-include ../makefile.init

RM := rm -rf

# All of the sources participating in the build are defined here
-include sources.mk
-include src/subdir.mk
-include emlib/subdir.mk
-include CMSIS/EFR32FG12P/subdir.mk
-include subdir.mk
-include objects.mk

ifneq ($(MAKECMDGOALS),clean)
ifneq ($(strip $(C_DEPS)),)
-include $(C_DEPS)
endif
endif

-include ../makefile.defs

OPTIONAL_TOOL_DEPS := \
$(wildcard ../makefile.defs) \
$(wildcard ../makefile.init) \
$(wildcard ../makefile.targets) \


BUILD_ARTIFACT_NAME := BRD4253A_EFR32FG12P_i2c
BUILD_ARTIFACT_EXTENSION := axf
BUILD_ARTIFACT_PREFIX :=
BUILD_ARTIFACT := $(BUILD_ARTIFACT_PREFIX)$(BUILD_ARTIFACT_NAME)$(if $(BUILD_ARTIFACT_EXTENSION),.$(BUILD_ARTIFACT_EXTENSION),)

# Add inputs and outputs from these tool invocations to the build variables 

# All Target
all: main-build

# Main-build Target
main-build: BRD4253A_EFR32FG12P_i2c.axf

# Tool invocations
BRD4253A_EFR32FG12P_i2c.axf: $(OBJS) $(USER_OBJS) makefile objects.mk $(OPTIONAL_TOOL_DEPS)
	@echo 'Building target: $@'
	@echo 'Invoking: GNU ARM C Linker'
	arm-none-eabi-gcc -g -gdwarf-2 -mcpu=cortex-m4 -mthumb -T "BRD4253A_EFR32FG12P_i2c.ld" -Wl,--no-warn-rwx-segments -Xlinker --gc-sections -Xlinker -Map="BRD4253A_EFR32FG12P_i2c.map" -mfpu=fpv4-sp-d16 -mfloat-abi=softfp --specs=nano.specs -o BRD4253A_EFR32FG12P_i2c.axf "./CMSIS/EFR32FG12P/startup_efr32fg12p.o" "./CMSIS/EFR32FG12P/system_efr32fg12p.o" "./emlib/em_cmu.o" "./emlib/em_core.o" "./emlib/em_emu.o" "./emlib/em_gpio.o" "./emlib/em_i2c.o" "./emlib/em_system.o" "./src/main_efr.o" "./src/sl_syscalls.o" -Wl,--start-group -lgcc -lc -lnosys -Wl,--end-group
	@echo 'Finished building target: $@'
	@echo ' '

	@echo 'Building out file: BRD4253A_EFR32FG12P_i2c.out'
	arm-none-eabi-objcopy "BRD4253A_EFR32FG12P_i2c.axf" "BRD4253A_EFR32FG12P_i2c.out"
	@echo ' '

	@echo 'Building hex file: BRD4253A_EFR32FG12P_i2c.hex'
	arm-none-eabi-objcopy -O ihex "BRD4253A_EFR32FG12P_i2c.axf" "BRD4253A_EFR32FG12P_i2c.hex"
	@echo ' '

	@echo 'Building bin file: BRD4253A_EFR32FG12P_i2c.bin'
	arm-none-eabi-objcopy -O binary "BRD4253A_EFR32FG12P_i2c.axf" "BRD4253A_EFR32FG12P_i2c.bin"
	@echo ' '

	@echo 'Building s37 file: BRD4253A_EFR32FG12P_i2c.s37'
	arm-none-eabi-objcopy -O srec "BRD4253A_EFR32FG12P_i2c.axf" "BRD4253A_EFR32FG12P_i2c.s37"
	@echo ' '

	@echo 'Running size tool'
	arm-none-eabi-size "BRD4253A_EFR32FG12P_i2c.axf" -A
	@echo ' '

# Other Targets
clean:
	-@$(foreach FILE, $(EXECUTABLES), $(shell $(RM) $(FILE)) )
	-@$(foreach FILE, $(OBJS), $(shell $(RM) $(FILE)) )
	-@$(foreach FILE, $(C_DEPS), $(shell $(RM) $(FILE)) )
	-$(RM) BRD4253A_EFR32FG12P_i2c.axf
	-@echo ' '

.PHONY: all clean dependents main-build

-include ../makefile.targets
