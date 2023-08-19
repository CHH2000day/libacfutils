/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*
 * Copyright 2023 Saso Kiselkov. All rights reserved.
 */

use build_target::*;

fn main() {
	let plat: (&str, &str) = match target_os().unwrap() {
	Os::Windows => ("win64", "win-64"),
	Os::Linux => ("lin64", "linux-64"),
	Os::MacOs => ("mac64", "mac-64"),
	_ => unreachable!()
	};
	println!("cargo:rustc-link-search=native=../qmake/{}", plat.0);
	println!("cargo:rustc-link-lib=static=acfutils");
//	println!("cargo:rustc-link-lib=static=crypto");
//	println!("cargo:rustc-link-lib=static=ssl");
//	println!("cargo:rustc-link-lib=static=curl");
//	println!("cargo:rustc-link-lib=static=z");
}
