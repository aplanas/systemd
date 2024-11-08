/* SPDX-License-Identifier: BSD-3-Clause */

/*
 * Based on Nikita Travkin's dtbloader implementation.
 * Copyright (c) 2024 Nikita Travkin <nikita@trvn.ru>
 *
 * https://github.com/TravMurav/dtbloader/blob/main/src/chid.c
 */

/*
 * Based on Linaro dtbloader implementation.
 * Copyright (c) 2019, Linaro. All rights reserved.
 *
 * https://github.com/aarch64-laptops/edk2/blob/dtbloader-app/EmbeddedPkg/Application/ConfigTableLoader/CHID.c
 */

#include "chid.h"
#include "chid-fundamental.h"
#include "efi.h"
#include "sha1-fundamental.h"
#include "smbios.h"
#include "util.h"

/**
 * smbios_to_hashable_string() - Convert ascii smbios string to stripped char16_t.
 */
static char16_t *smbios_to_hashable_string(const char *str) {
        if (!str)
                /* User of this function is expected to free the result. */
                return xnew0(char16_t, 1);

        /*
         * We need to strip leading and trailing spaces, leading zeroes.
         * See fwupd/libfwupdplugin/fu-hwids-smbios.c
         */
        while (*str == ' ')
                str++;

        while (*str == '0')
                str++;

        size_t len = strlen8(str);

        while (len > 0 && str[len - 1] == ' ')
                len--;

        return xstrn8_to_16(str, len);
}

/* This has to be in a struct due to _cleanup_ in populate_board_chids */
typedef struct SmbiosInfo {
        const char16_t *smbios_fields[_CHID_SMBIOS_FIELDS_MAX];
} SmbiosInfo;

static void smbios_info_populate(SmbiosInfo *ret_info) {
        static RawSmbiosInfo raw = {};
        static bool raw_info_populated = false;

        if (!raw_info_populated) {
                smbios_raw_info_populate(&raw);
                raw_info_populated = true;
        }

        ret_info->smbios_fields[CHID_SMBIOS_MANUFACTURER] = smbios_to_hashable_string(raw.manufacturer);
        ret_info->smbios_fields[CHID_SMBIOS_PRODUCT_NAME] = smbios_to_hashable_string(raw.product_name);
        ret_info->smbios_fields[CHID_SMBIOS_PRODUCT_SKU] = smbios_to_hashable_string(raw.product_sku);
        ret_info->smbios_fields[CHID_SMBIOS_FAMILY] = smbios_to_hashable_string(raw.family);
        ret_info->smbios_fields[CHID_SMBIOS_BASEBOARD_PRODUCT] = smbios_to_hashable_string(raw.baseboard_product);
        ret_info->smbios_fields[CHID_SMBIOS_BASEBOARD_MANUFACTURER] = smbios_to_hashable_string(raw.baseboard_manufacturer);
}

static void smbios_info_done(SmbiosInfo *info) {
        FOREACH_ELEMENT(i, info->smbios_fields)
                free(i);
}

static EFI_STATUS populate_board_chids(EFI_GUID ret_chids[static CHID_TYPES_MAX]) {
        _cleanup_(smbios_info_done) SmbiosInfo info = {};

        if (!ret_chids)
                return EFI_INVALID_PARAMETER;

        smbios_info_populate(&info);
        chid_calculate(info.smbios_fields, ret_chids);

        return EFI_SUCCESS;
}

EFI_STATUS chid_match(const void *hwid_buffer, size_t hwid_length, const Device **ret_device) {
        EFI_STATUS status;

        if ((uintptr_t) hwid_buffer % alignof(Device) != 0)
                return EFI_INVALID_PARAMETER;

        const Device *devices = ASSERT_PTR(hwid_buffer);

        EFI_GUID chids[CHID_TYPES_MAX] = {};
        static const size_t priority[] = { 3, 6, 8, 10, 4, 5, 7, 9, 11 }; /* From most to least specific. */

        status = populate_board_chids(chids);
        if (EFI_STATUS_IS_ERROR(status))
                return log_error_status(status, "Failed to populate board CHIDs: %m");

        size_t n_devices = 0;

        /* Count devices and check validity */
        for (; (n_devices + 1) * sizeof(*devices) < hwid_length;) {
                if (devices[n_devices].struct_size == 0)
                        break;
                if (devices[n_devices].struct_size != sizeof(*devices))
                        return EFI_UNSUPPORTED;
                n_devices++;
        }

        if (n_devices == 0)
                return EFI_NOT_FOUND;

        FOREACH_ELEMENT(i, priority)
                FOREACH_ARRAY(dev, devices, n_devices) {
                        /* Can't take a pointer to a packed struct member, so copy to a local variable */
                        EFI_GUID chid = dev->chid;
                        if (efi_guid_equal(&chids[*i], &chid)) {
                                *ret_device = dev;
                                return EFI_SUCCESS;
                        }
                }

        return EFI_NOT_FOUND;
}