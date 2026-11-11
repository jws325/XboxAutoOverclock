/*
	xbox_oc
	----------------
	Copyright (C) 2022-2023 wutno (https://github.com/GXTX)
	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.
	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.
	You should have received a copy of the GNU General Public License along
	with this program; if not, write to the Free Software Foundation, Inc.,
	51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include <hal/debug.h>
#include <hal/video.h>
#include <hal/xbox.h>
#include <windows.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CPU_BASE_MULTIPLIER 5.5f
#define BASE_CLOCK_INT 16667
#define BASE_CLOCK_FLOAT 16.667f

ULONG original_fsb, wanted_fsb, hidden_fsb;
ULONG original_nvclk, wanted_nvclk;
int original_m, original_n, original_p, original_mp;
int wanted_m, wanted_n, wanted_p, wanted_mp;
ULONG pci_buff, cpu_coeff;

// https://github.com/WulfyStylez/XBOverclock
void calc_clock_params(int clk, int *n, int *m)
{
	int work1, work2, work4;

	work1 = clk / BASE_CLOCK_FLOAT;
	work2 = (clk * 2) / BASE_CLOCK_FLOAT;
	work4 = (clk * 4) / BASE_CLOCK_FLOAT;

	if (work2 * 2 != work4) {
		*n = work4;
		*m = 4;
	} else if (work1 * 2 != work2) {
		*n = work2;
		*m = 2;
	} else {
		*n = work1;
		*m = 1;
	}
}

// Returns true when two config keys differ only in letter case.
static int configKeyEquals(const char *a, const char *b)
{
	while (*a && *b) {
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
			return 0;
		++a;
		++b;
	}
	return *a == '\0' && *b == '\0';
}

// Blank, malformed and out-of-range settings do not override existing clocks.
// Clock values in config.ini are positive, whole MHz.
static int configClockMHz(char *text, int *mhz)
{
	char *end;
	unsigned long value;

	while (isspace((unsigned char)*text))
		++text;
	if (!isdigit((unsigned char)*text))
		return 0;

	value = strtoul(text, &end, 10);
	while (isspace((unsigned char)*end))
		++end;
	if (*end && *end != ';' && *end != '#')
		return 0;
	if (value == 0 || value > 5000)
		return 0;

	*mhz = (int)value;
	return 1;
}

// Read startup clock targets after the hardware registers have been read.
// D: points at the launched title's directory on the original Xbox.
static int loadClockConfig(void)
{
	FILE *file = fopen("D:\\config.ini", "r");
	char line[128];
	int cpu_mhz = 0, ram_mhz = 0, nvclk_mhz = 0;

	if (!file)
		file = fopen("config.ini", "r");
	if (!file) {
		debugPrint("config.ini not found; clocks unchanged.\n");
		return 0;
	}

	while (fgets(line, sizeof(line), file)) {
		char *key = line;
		char *equal, *end;
		int value;

		while (isspace((unsigned char)*key))
			++key;
		if (*key == '\0' || *key == ';' || *key == '#' || *key == '[')
			continue;
		equal = strchr(key, '=');
		if (!equal)
			continue;
		*equal++ = '\0';
		end = key + strlen(key);
		while (end > key && isspace((unsigned char)end[-1]))
			*--end = '\0';
		if (!configClockMHz(equal, &value))
			continue;

		if (configKeyEquals(key, "CPU_SPEED"))
			cpu_mhz = value;
		else if (configKeyEquals(key, "RAM_SPEED"))
			ram_mhz = value;
		else if (configKeyEquals(key, "NVCLK_SPEED"))
			nvclk_mhz = value;
	}
	fclose(file);

	// CPU multiplier is fixed in this tool. Find the closest whole-MHz FSB.
	// Do not alter the CPU/FSB if CPU_SPEED was left blank.
	if (cpu_mhz) {
		ULONG fsb = (ULONG)((cpu_mhz * 2 + 5) / 11);
		int n, m;
		calc_clock_params((int)fsb, &n, &m);
		if (fsb > 0 && n >= 1 && n <= 255 && m >= 1 && m <= 255)
			wanted_fsb = fsb;
	}

	// The existing UI changes RAM through its four-bit memory divider.
	// Choose the best divider for the selected FSB, leaving FSB untouched
	// when CPU_SPEED is blank (RAM and CPU cannot be fully independent).
	if (ram_mhz && wanted_p > 0) {
		int n, m, divider, best_divider = wanted_mp;
		double best_error = 1.0e30;
		if (wanted_fsb == original_fsb) {
			n = original_n;
			m = original_m;
		} else {
			calc_clock_params((int)wanted_fsb, &n, &m);
		}
		if (n >= 1 && n <= 255 && m >= 1 && m <= 255) {
			for (divider = 1; divider <= 15; ++divider) {
				double ram_clock = ((double)BASE_CLOCK_FLOAT / m) * wanted_p * n / divider;
				double error = ram_clock - ram_mhz;
				if (error < 0)
					error = -error;
				if (error < best_error) {
					best_error = error;
					best_divider = divider;
				}
			}
			wanted_mp = best_divider;
		}
	}

	if (nvclk_mhz) {
		int n, m;
		// The original NVCLK apply path calls calc_clock_params((NVCLK+1)*2).
		calc_clock_params((nvclk_mhz + 1) * 2, &n, &m);
		if (n >= 1 && n <= 255 && m >= 1 && m <= 255)
			wanted_nvclk = (ULONG)nvclk_mhz;
	}
	return 1;
}

ULONG get_GPU_frequency()
{
    ULONG nvclk_reg, current_nvclk;
    nvclk_reg = *((volatile ULONG *)0xFD680500);

    current_nvclk = BASE_CLOCK_INT * ((nvclk_reg & 0xFF00) >> 8);
    current_nvclk /= 1 << ((nvclk_reg & 0x70000) >> 16);
    current_nvclk /= nvclk_reg & 0xFF;
    current_nvclk /= 1000;

    return current_nvclk;
}

void outputClocks()
{
	if (!wanted_mp)
		wanted_mp++;
	if (wanted_fsb == original_fsb) {
		wanted_n = original_n;
		wanted_m = original_m;
	} else {
		calc_clock_params(wanted_fsb, &wanted_n, &wanted_m);
	}
	hidden_fsb = (BASE_CLOCK_FLOAT / wanted_m) * wanted_n;

	ULONG cpu_clk = (int)(wanted_fsb * CPU_BASE_MULTIPLIER);
	ULONG mem_clk = ((BASE_CLOCK_FLOAT / wanted_m) * (wanted_p * 2 * wanted_n)) / (2 * wanted_mp);

	cpu_coeff = (pci_buff & ~0x00FFFFFF) | (wanted_mp << 20) | (wanted_p << 16) | (wanted_n << 8) | wanted_m;

	debugPrint("\nFSB: %03luMHz, CPU: %03luMHz, RAM: %3luMHz\n", wanted_fsb, cpu_clk, mem_clk);
	debugPrint("NVCLK : %03luMHz\n", wanted_nvclk);
}

static inline __attribute__((always_inline)) void writeCPUClocks(ULONG coeff)
{
	// wait and disable interrupts
	KeEnterCriticalRegion();
	KeStallExecutionProcessor(100000);
	__asm__("cli\n\t"
		"sfence\n\t"
		"nop\n\t"
		"nop\n\t"
		"nop\n\t"
		"nop\n\t"
		"nop\n\t"
	);

	HalReadWritePCISpace(0, 0x60, 0x6C, &coeff, sizeof(coeff), TRUE);

	// wait and enable interrupts
	__asm__("nop\n\t"
		"nop\n\t"
		"nop\n\t"
		"nop\n\t"
		"nop\n\t"
		"sfence\n\t"
		"sti\n\t"
	);
	KeStallExecutionProcessor(100000);
	KeLeaveCriticalRegion();
}

int main(void)
{
	XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);

	// Read the current FSB settings; no SDL or controller is required.
	pci_buff = 0;
	HalReadWritePCISpace(0, 0x60, 0x6C, &pci_buff, sizeof(pci_buff), FALSE);

	original_m  = wanted_m  = pci_buff & 0xFF;
	original_n  = wanted_n  = (pci_buff >> 8) & 0xFF;
	original_p  = wanted_p  = (pci_buff >> 16) & 0xF;
	original_mp = wanted_mp = (pci_buff >> 20) & 0xF;

	// Fail closed if the hardware clock registers cannot be decoded.
	if (!original_m || !original_n || !original_mp) {
		debugPrint("Invalid hardware clock dividers; no changes applied.\n");
		Sleep(3000);
		return 1;
	}

	original_fsb = (BASE_CLOCK_FLOAT / wanted_m) * wanted_n;
	wanted_fsb = original_fsb;

	if (!(*((volatile ULONG *)0xFD680500) & 0xFF)) {
		debugPrint("Invalid NVCLK divider; no changes applied.\n");
		Sleep(3000);
		return 1;
	}
	original_nvclk = get_GPU_frequency();
	wanted_nvclk = original_nvclk;

	if (!loadClockConfig()) {
		Sleep(2000);
		return 0;
	}

	outputClocks();

	// Apply only if at least one requested clock differs from the hardware.
	if (wanted_nvclk == original_nvclk &&
	    wanted_fsb == original_fsb &&
	    wanted_mp == original_mp) {
		debugPrint("No clock changes requested.\n");
		Sleep(2000);
		return 0;
	}

	debugPrint("Applying config.ini clocks automatically.\n");
	debugPrint("WARNING: Incorrect clocks can freeze or damage your Xbox.\n");
	Sleep(1000);

	int n, m;
	ULONG coeff;

	if (wanted_nvclk != original_nvclk) {
		// Preserve the original NVCLK calculation and register write sequence.
		calc_clock_params((++wanted_nvclk) * 2, &n, &m);
		debugClearScreen();
		debugPrint("Setting NVCLK to: %03dMHz\n", (BASE_CLOCK_INT * n / m) / 2 / 1000);

		coeff = (*((volatile ULONG *)0xFD680500) & ~0x0000FFFF) | (n << 8) | m;
		Sleep(500);
		*((volatile ULONG *)0xFD680500) = coeff;
		Sleep(500);
	}

	// When raising the FSB, apply a requested memory divider change first.
	if (wanted_mp != original_mp && wanted_fsb > original_fsb) {
		debugClearScreen();
		debugPrint("Setting MemDiv to: %d\n", wanted_mp);
		coeff = (pci_buff & ~0x00F00000) | (wanted_mp << 20);
		writeCPUClocks(coeff);
	}

	if (wanted_fsb != original_fsb) {
		debugClearScreen();
		debugPrint("Setting FSB to: %03dMHz\n", (int)(BASE_CLOCK_FLOAT * wanted_n / wanted_m));
		writeCPUClocks(cpu_coeff);
	} else if (wanted_mp != original_mp) {
		// RAM-only updates must be written even when FSB stays the same.
		debugClearScreen();
		debugPrint("Setting MemDiv to: %d\n", wanted_mp);
		coeff = (pci_buff & ~0x00F00000) | (wanted_mp << 20);
		writeCPUClocks(coeff);
	}

	// Same quick reboot behavior as the original Start button.
	HalReturnToFirmware(HalQuickRebootRoutine);
	return 0; // unreachable
}
