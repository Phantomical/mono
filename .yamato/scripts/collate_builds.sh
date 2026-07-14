#!/bin/bash
set -euxo pipefail

repo_dir="realpath $(dirname "$0")/../.."

sudo apt-get install -qy unzip

osarch=`uname -s`-`uname -m`
case $osarch in
	Linux-x86_64)
		mkdir -p "$repo_dir/artifacts/libarchive"
		pushd "$repo_dir/artifacts/libarchive"

		if [ ! -e bsdtar ]
		then
			wget -q https://public-stevedore.unity3d.com/r/public/bsdtar-linux-x64/3.8.1_28985563_1365df71ca0a504c6949b1cf2e257caa6b0cacbac4b2b15f49ce75da6699bc87.zip
			unzip 3.8.1_28985563_1365df71ca0a504c6949b1cf2e257caa6b0cacbac4b2b15f49ce75da6699bc87.zip bsdtar
		fi

		PATH=`pwd`:$PATH
		popd
		;;
	*)
		echo "Error: this script does not support $osarch"
		exit 1
	;;
esac

PATH=`pwd`:$PATH

perl external/buildscripts/collect_allbuilds.pl
pwd
ls -al
