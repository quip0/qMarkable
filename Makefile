# Cross-compiles the tracker for the reMarkable Paper Pure (aarch64) with zig.
#   brew install zig
CC = zig cc -target aarch64-linux-musl
CFLAGS = -O2 -static -s -Wall -Wextra

tracker: tracker/qmarkable-trackerd

tracker/qmarkable-trackerd: tracker/qmarkable-trackerd.c
	$(CC) $(CFLAGS) -o $@ $< -lm

clean:
	rm -f tracker/qmarkable-trackerd

.PHONY: tracker clean
