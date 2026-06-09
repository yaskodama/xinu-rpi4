// llm/blob.S — bake the tiny LLM (Karpathy stories260K, llama2.c legacy
// format) and its tokenizer into the kernel image as .rodata blobs.  Paths
// are relative to the build CWD (compile/).
.section .rodata
.balign 8
.global model_data
model_data:
#ifdef LLM_NO_MODEL
// Pi 5 build: omit the ~1 MB model so the loaded image stays small enough
// to boot (the full blob pushes the image past the point where the Pi 5
// firmware/BL31 hand-off leaves the kernel running).  The symbols still
// exist so llm.c links; the model is just an empty placeholder, making the
// llm/chat commands inert on the Pi 5.
.byte 0
#else
.incbin "../llm/stories260K.bin"
#endif
.global model_data_end
model_data_end:

.balign 8
.global tok_data
tok_data:
#ifdef LLM_NO_MODEL
.byte 0
#else
.incbin "../llm/tok512.bin"
#endif
.global tok_data_end
tok_data_end:
