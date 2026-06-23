// os_wifi_stub.S — 1-byte stand-ins for the WiFi firmware blob (wifi_fw.c,
// ~0.55 MB).  Used by OS1 only (OS1 drops WiFi).  OS2 links the real wifi_fw.o.
.section .rodata
.balign 4
.global wifi_fw_bin
.global wifi_fw_bin_end
.global wifi_nvram_txt
.global wifi_nvram_txt_end
.global wifi_clm_blob
.global wifi_clm_blob_end
wifi_fw_bin:      .byte 0
wifi_fw_bin_end:
wifi_nvram_txt:   .byte 0
wifi_nvram_txt_end:
wifi_clm_blob:    .byte 0
wifi_clm_blob_end:
