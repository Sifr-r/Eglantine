# Next upstream fixes

| Issue | Code ownership | Check |
| --- | --- | --- |
| #777 | The OS process wrapper and kernel object registry own failed-load cleanup. | Cover failure before and after process initialization, followed by allocation or registry use. Preserve successful-load and process-signaling behavior. |
| #813 | Qt migration owns rewriting paths into the migrated directory. | Exercise Copy and Move, legacy Eden paths, custom and similarly named paths, and the derived save directory. |
| #807 | `src/video_core/gpu_split.h` owns the separation of front and back thread state. | Verify layout separation and compile the GPU code. Performance requires a separate Mario Kart World measurement. |
| #878 | The NUP metadata editor owns archive rewriting. | Edit a synthetic NUP whose metadata precedes the blobs, then reopen and verify all original payloads. Cover repeated edits and preserve the normal archive layout. |
