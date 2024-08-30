/*
 * SPDX-License-Identifier:     GPL-2.0+
 *
 * (C) Copyright 2022 Rockchip Electronics Co., Ltd
 */

#include <common.h>
#include <asm/io.h>
#include <dwc3-uboot.h>
#include <usb.h>

DECLARE_GLOBAL_DATA_PTR;

#define CRU_BASE		0xFF3B2000
#define CRU_SOFTRST_CON04	0x0A10

#ifdef CONFIG_USB_DWC3
static struct dwc3_device dwc3_device_data = {
	.maximum_speed = USB_SPEED_HIGH,
	.base = 0xffb00000,
	.dr_mode = USB_DR_MODE_PERIPHERAL,
	.index = 0,
	.dis_u2_susphy_quirk = 1,
	.usb2_phyif_utmi_width = 16,
};

int usb_gadget_handle_interrupts(void)
{
	dwc3_uboot_handle_interrupt(0);
	return 0;
}

#ifdef CONFIG_SUPPORT_USBPLUG
static void usb_reset_otg_controller(void)
{
	writel(0x1 << 7 | 0x1 << 23, CRU_BASE + CRU_SOFTRST_CON04);
	mdelay(1);
	writel(0x0 << 7 | 0x1 << 23, CRU_BASE + CRU_SOFTRST_CON04);

	mdelay(1);
}
#endif

int board_usb_init(int index, enum usb_init_type init)
{
#ifdef CONFIG_SUPPORT_USBPLUG
	usb_reset_otg_controller();
#endif
	writel(0x01ff0000, 0xff000050); /* Resume usb2 phy to normal mode */

	return dwc3_uboot_init(&dwc3_device_data);
}
#endif

#ifndef CONFIG_SPL_BUILD
static int do_load_version(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[])
{
        uint32_t vlen = 0;
        char cmd_mod[128] = {0};

        run_command("run distro_bootcmd;", -1);
        printf("Loading order: tf - emmc\n");
        run_command("checkconf tf;", -1);
        if(!run_command("load mmc 1:1 ${loadaddr} ${bootdir}${image}", 0)) {
                run_command("load mmc 1:1 ${initrd_addr} ${bootdir}${rd_file}", -1);
                vlen = simple_strtoul(env_get("filesize"), NULL, 16);
                run_command("load mmc 1:1 ${fdt_addr_r} ${bootdir}${fdt_file}", -1);
                sprintf(cmd_mod, "bootz ${loadaddr} ${initrd_addr}:%x ${fdt_addr_r}", vlen);
                run_command(cmd_mod, -1);
                return 0;
        }
        run_command("checkconf mmc;", -1);
        if(!run_command("load mmc 0:4 ${loadaddr} ${bootdir}${image}", 0)) {
                run_command("load mmc 0:4 ${initrd_addr} ${bootdir}${rd_file}", -1);
                vlen = simple_strtoul(env_get("filesize"), NULL, 16);
                run_command("load mmc 0:4 ${fdt_addr_r} ${bootdir}${fdt_file}", -1);
                sprintf(cmd_mod, "bootz ${loadaddr} ${initrd_addr}:%x ${fdt_addr_r}", vlen);
                run_command(cmd_mod, -1);
                return 0;
        }

        mdelay(1000);

        return 0;
}

U_BOOT_CMD(loadver, 4, 0, do_load_version,
           "Load version",
           "Booting\n"
);

void init_board_env(void)
{
        char *temp = NULL;

	run_command("c read;", -1);

        env_set("board_name", "CoolPi Nano RV1106");

	temp = env_get("fixmac");
	if(temp) {
		if(strncmp(env_get("fixmac"), "yes", 3)) {
			env_set("fixmac", "yes");
			run_command("c write;", -1);
		}
	} else {
		env_set("fixmac", "yes");
		run_command("c write;", -1);
	}
	run_command("c read;", -1);
        env_set("board", "coolpi");
        env_set("soc", "rv1106");
}
#endif
