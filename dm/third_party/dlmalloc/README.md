# dlmalloc

`malloc.c` from https://gee.cs.oswego.edu/pub/misc/malloc.c: Doug Lea's malloc 2.8.6, re-licensed in 2023 under MIT-0 (`LICENSE`), unmodified (SHA-256 `be7be56b40b4b31417e74e77687f9b96127ea6cae534cf236aee40a4c739508d`). With hibernation on, the host runs an Unreal Engine game's heap on its `mspace` API inside the fixed address layout, so that the heap, the allocator's own state included, is part of the snapshot: `dm/host/hle_ueheap.c` includes `malloc.c` with its configuration (mspaces only, no morecore, segments from a fixed region of the reservation).
