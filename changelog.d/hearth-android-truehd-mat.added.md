**Hearth for Android plays TrueHD to a receiver as MAT.** A server that sees `truehd` in the app's support object sends the stream's access units, 24 to a chunk;
the pass-through output packs each chunk into a 61,440-byte MAT burst and the app writes it to an `ENCODING_IEC61937` track on the 7.1 mask. The app lists `truehd`
only on a route that takes the track. It is built and tested on the host against a model of the device; it has not run on a Shield.
