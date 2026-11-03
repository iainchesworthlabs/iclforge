**Hearth reads a Matroska or MP4 file's TrueHD track where it lies, a window at a time.** A film remux's lossless track (about
4.5 GB for two hours) was copied into memory beside the file; now `iclforge::apps::index_container_track` finds where its frames lie
(`libs/containers`' Matroska `Reader::push_at` and MP4 `demux_seekable_at` say at what offset) and `ExtentSource` reads the track back by
offset, so a bare `.thd` file and a container's track play the same way. A Transport Stream is still read whole, and one over 1 GiB is
refused by name. A test reads a generated 160 MiB file through a source that refuses reads over 10 MiB.
