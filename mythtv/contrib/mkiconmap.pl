#!/usr/bin/perl
use strict;
use Getopt::Long;
use File::Glob ':glob';
use File::Basename;
use URI;

my $version = "0.1";
my $scriptname = "$0";

my $SHARE_DIRECTORY="/tmp";
my $opt_help = 0;
my $opt_grab = 1;
my $opt_output_file = "iconmap.xml";
my $opt_zip = "";
my $opt_lineup = "";
my $opt_noprefix = 0;
my $opt_nograb = 0;

my $gen_prefix = "dni_";

my $ICONGRABBER="tv_grab_na_icons";

my %callsign_to_url;

my %master_urls;

my %primary_stubs;

my $quiet = 0;

if (! -t STDOUT) {
	$quiet = 1;
}

sub print_version {
	print("$scriptname Version $version\n");
}

sub usage {
	my ($retval) = @_;
	print_version();
	print("usage:
	--help               This.
	--share <directory>  Directory to download URL files to.
	--out <filename>     File to save XML output to (default: $opt_output_file)
	--grab               Call the icon grabber.

	example:
		$scriptname --grab --share=/tmp --out=iconmap.xml
		
");
	exit $retval;
}

sub qprint {
	my ($string) = @_;
	if (!$quiet) {
		print($string);
	}
}

sub qsystem {
	my ($command) = @_;
	if ($quiet) {
		$command = $command . " > /dev/null 2>&1";
	}
	system($command);
}

GetOptions(
	'help'     => \$opt_help,
	'share=s'  => \$SHARE_DIRECTORY,
	'out=s'  => \$opt_output_file,
	'grab'  => \$opt_grab,
	'zip=s' => \$opt_zip,
	'lineup=s' => \$opt_lineup,
	'noprefix' => \$opt_noprefix,
	'nograb' => \$opt_nograb,
) or usage(1);

if ($opt_help) {
	usage(1);
}

if ($opt_noprefix) {
	$gen_prefix = "";
}

if ($opt_nograb) {
	$opt_grab = 0;
}

if ($opt_grab) {
	if ($quiet) {
		print("Error cannot grab quietly... yet\n");
		exit 1;
	}
	qprint("Calling $ICONGRABBER...\n");
	my $command = "$ICONGRABBER --share=$SHARE_DIRECTORY --links";
	if (length($opt_zip)) {
		$command = "$command --zip=$opt_zip";
	}
	if (length($opt_lineup)) {
		$command = "$command --lineup=$opt_lineup";
	}
	qsystem($command);
}

open(XMLOUT, "> $opt_output_file") or die("Error unable to open outfile file: '$opt_output_file' for writting.");

my @url_files = <$SHARE_DIRECTORY/icons/*.url>;

while (my $url_file = shift(@url_files)) {
	handle_url_file($url_file);
}

sub split_url_from_file {
	my ($url) = @_;
	if ($url =~ m/(.*)\/([^\/]+)/) {
		return ($1, $2);
	}
	return ($url, "");
}

sub handle_url_file {
	my ($filename) = (@_);

	my $callsign = basename($filename, ".url");

	open(IURL, "< $filename");
	while (my $url = <IURL>) {
		chomp($url);
		if (length($url)) {
			my ($base_url, $icon_filename) = split_url_from_file($url);
			$master_urls{$base_url} = 1;
			$callsign_to_url{$callsign} = $url;
		}
	}
}

sub stubbase_from_host {
	my ($hostname) = @_;
	my @hostparts = split(/\./, $hostname);

	if (scalar(@hostparts) > 1) {
		splice(@hostparts, 0, 1);
		return shift(@hostparts);
	} else {
		return $hostname;
	}
	return "generic";
}

sub get_unique_stub {
	my ($basestub) = @_;

	if (exists $primary_stubs{$basestub}) {
		my $basecount = $primary_stubs{$basestub} + 0;
		$basecount++;
		my $newbase = "$basestub$basecount";
		$primary_stubs{$newbase} = 0;
		return $newbase;
	} else {
		$primary_stubs{$basestub} = 0;
	}
	return $basestub;
}

#make stub names for all the URLs we found
foreach my $url_key (keys(%master_urls)) {
	my $uri = URI->new($url_key);
	my $hostname = $uri->host();
	my $stubbase = "$gen_prefix" . stubbase_from_host($hostname);

	$master_urls{$url_key} = get_unique_stub($stubbase);
}

sub stubbed_url {
	my ($full_url) = @_;
	my ($url, $icon_file) = split_url_from_file($full_url);
	if (exists $master_urls{$url}) {
		return "[" . $master_urls{$url} . "]/$icon_file";
	}
	return $full_url;
}

qprint("Generating $opt_output_file...\n");
print(XMLOUT "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
print(XMLOUT "<!-- generated by $scriptname -->\n");
print(XMLOUT "<iconmappings>\n");

foreach my $callsign_key (keys(%callsign_to_url)) {
	print(XMLOUT "\t<callsigntonetwork>\n");
	print(XMLOUT "\t\t<callsign>$callsign_key</callsign>\n");
	print(XMLOUT "\t\t<network>$gen_prefix$callsign_key</network>\n");
	print(XMLOUT "\t</callsigntonetwork>\n");
}

print(XMLOUT "\n");

foreach my $callsign_key (keys(%callsign_to_url)) {
	print(XMLOUT "\t<networktourl>\n");
	print(XMLOUT "\t\t<network>$gen_prefix$callsign_key</network>\n");
	my $full_url = $callsign_to_url{$callsign_key};
	print(XMLOUT "\t\t<url>" . stubbed_url($full_url) . "</url>\n");
	print(XMLOUT "\t</networktourl>\n");
}

print(XMLOUT "\n");

foreach my $stub_key (keys(%master_urls)) {
	print(XMLOUT "\t<baseurl>\n");
	print(XMLOUT "\t\t<stub>" . $master_urls{$stub_key} . "</stub>\n");
	print(XMLOUT "\t\t<url>$stub_key</url>\n");
	print(XMLOUT "\t</baseurl>\n");
}

print(XMLOUT "</iconmappings>\n");
