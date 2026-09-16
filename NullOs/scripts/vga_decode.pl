#!/usr/bin/perl
# Decode qemu monitor 'xp /Nbx 0xb8000' hex dump back into VGA text.
# Reads hex string on stdin (raw bytes), emits printable chars at even offsets.
binmode STDIN;
local $/;
my $data = <STDIN>;
for (my $i = 0; $i + 1 < length($data); $i += 2) {
    my $c = substr($data, $i, 1);
    print $c if $c =~ /[\x20-\x7e]/;
}
