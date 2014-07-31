#!/usr/bin/perl -w

use strict;

sub PASS
{
    my ($format, $args) = @_;
    if(!defined $args)
    { $args = []; }
    printf("PASS \"$format\"\n", @$args);
}

sub FAIL
{
    my ($format, $args) = @_;
    if(!defined $args)
    { $args = []; }
    printf("FAIL \"$format\"\n", @$args);
}

my $pass_string = shift @ARGV;
my $fail_string = shift @ARGV;

if(0 == system(@ARGV))
{
    PASS($pass_string);
}
else
{
    FAIL($fail_string);
}
exit 0;

