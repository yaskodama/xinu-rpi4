// os_llm_stub.S — 1-byte stand-ins for the baked LLM model (blob.S, ~1.0 MB).
// Used by OS1 and OS2 (both minimal kernels drop the LLM; the small dormant
// llm.c still links against these symbols).
.section .rodata
.balign 4
.global model_data
.global model_data_end
.global tok_data
.global tok_data_end
model_data:   .byte 0
model_data_end:
tok_data:     .byte 0
tok_data_end:
