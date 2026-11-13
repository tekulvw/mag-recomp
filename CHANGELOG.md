# Changelog

## [0.5.1](https://github.com/tekulvw/mag-recomp/compare/v0.5.0...v0.5.1) (2026-11-13)


### Bug Fixes

* **release:** require releasable titles for SDK updates ([#182](https://github.com/tekulvw/mag-recomp/issues/182)) ([4f908f9](https://github.com/tekulvw/mag-recomp/commit/4f908f98a666fa8fbbb10fd0c3a521286c74d40d))
* **render:** declare only sampled fragment texture units ([#183](https://github.com/tekulvw/mag-recomp/issues/183)) ([46cd235](https://github.com/tekulvw/mag-recomp/commit/46cd23574e89e30c6bd4132e9733774a5237c175))
* **sdk:** refresh reviewed runtime pin and coherence defaults ([59775de](https://github.com/tekulvw/mag-recomp/commit/59775deab94ac7f4c47cfb2bd899c2f1af28f54a))


### Performance

* **linux:** enable validated native geometry jobs by default ([#187](https://github.com/tekulvw/mag-recomp/issues/187)) ([fde31af](https://github.com/tekulvw/mag-recomp/commit/fde31aff45800dd18e0aaf0a344b3199c01329d2))
* **render:** index native scene and Vulkan resource lookups ([#186](https://github.com/tekulvw/mag-recomp/issues/186)) ([a05e05a](https://github.com/tekulvw/mag-recomp/commit/a05e05ac1e3497390b181ed7e64b21f2dedbb1a9))
* **render:** reuse retired Vulkan buffer allocations ([#184](https://github.com/tekulvw/mag-recomp/issues/184)) ([ba59663](https://github.com/tekulvw/mag-recomp/commit/ba596639c72cfdbef305ee898545802a965a5362))
* **windows:** cache getenv for the runtime and MAG sources ([#140](https://github.com/tekulvw/mag-recomp/issues/140)) ([f529e06](https://github.com/tekulvw/mag-recomp/commit/f529e062f968022ab9e91c1c2e06bc94ec421e2e))

## [0.5.0](https://github.com/tekulvw/mag-recomp/compare/v0.4.0...v0.5.0) (2026-11-10)


### Features

* **controls:** add the opt-in game controls adapter ([619d29a](https://github.com/tekulvw/mag-recomp/commit/619d29a4a4d685c31e51500e6b038f68f3ee3a36))
* **controls:** add the opt-in game controls adapter ([732276f](https://github.com/tekulvw/mag-recomp/commit/732276fcad963765561609370bde520b8444c6ed))
* **controls:** capture keyboard and mouse on Linux through SDL ([10b56fe](https://github.com/tekulvw/mag-recomp/commit/10b56feaeb59aadc1e116d4f218a4db5ecb17e75))
* **controls:** capture keyboard and mouse on Linux through SDL ([64d1dc8](https://github.com/tekulvw/mag-recomp/commit/64d1dc8daa6781a1324fb7cee0c22bf159469e3b))
* **controls:** capture keyboard and mouse on Windows through Raw Input ([7fbf410](https://github.com/tekulvw/mag-recomp/commit/7fbf41076aa1c7137916558f5abe6178f743e19a))
* **controls:** capture keyboard and mouse on Windows through Raw Input ([d9cd38c](https://github.com/tekulvw/mag-recomp/commit/d9cd38c3c663db13aefbc0f35ef8387dd3b8bf7f))
* **controls:** prepare the controls hook stage and build it from the installer recipe ([da9fa76](https://github.com/tekulvw/mag-recomp/commit/da9fa76aa9871be36b32454b5433dfde70a95e93))
* **controls:** prepare the controls hook stage and build it from the installer recipe ([1490dba](https://github.com/tekulvw/mag-recomp/commit/1490dba3f8590e6f31f84ee49d801e62edf159b4))
* **controls:** register the game controls catalog with the settings service ([68980fd](https://github.com/tekulvw/mag-recomp/commit/68980fd3174c0b438b38bc6ce5f8c50d0cc4784c))
* **controls:** register the game controls catalog with the settings service ([87394f7](https://github.com/tekulvw/mag-recomp/commit/87394f7dd93c6668dce34f6e96b329b729904dee))
* **controls:** trace mouse motion after each Linux mouse re-lock (opt-in) ([#161](https://github.com/tekulvw/mag-recomp/issues/161)) ([8604da9](https://github.com/tekulvw/mag-recomp/commit/8604da9451f65f15e091d6901e9dc7eda343e538))
* **extension:** opt-in payload override at the WWS policy payload call ([cc66a66](https://github.com/tekulvw/mag-recomp/commit/cc66a66012fbce3c8608322ebeb0cfd09d4236fc))
* **extension:** opt-in payload override at the WWS policy payload call ([786c8cd](https://github.com/tekulvw/mag-recomp/commit/786c8cd19ce4775dfbc4030df81bcb15485d1e2a))
* **spu:** opt-in native Edge geometry job through the payload override ([#174](https://github.com/tekulvw/mag-recomp/issues/174)) ([9344e2a](https://github.com/tekulvw/mag-recomp/commit/9344e2ad1bf68b2248558819853e190d3d21367d))


### Bug Fixes

* **controls:** confine the Linux mouse lock to the game window and log capture changes ([95b4fe6](https://github.com/tekulvw/mag-recomp/commit/95b4fe66fdb4f80a01f8429c7e62d01fd3d37058))
* **controls:** debounce the menu mouse-lock release ([#162](https://github.com/tekulvw/mag-recomp/issues/162)) ([ba1b093](https://github.com/tekulvw/mag-recomp/commit/ba1b093a30b04c45f008ec5be0881068fabc7ed4))
* **controls:** do not re-acquire a held mouse lock on every click ([#164](https://github.com/tekulvw/mag-recomp/issues/164)) ([b161974](https://github.com/tekulvw/mag-recomp/commit/b161974783079384ea6f109f3b3d791643f34ccf))
* **controls:** log Windows capture changes and ignore input from an unfocused window ([ed3fbf6](https://github.com/tekulvw/mag-recomp/commit/ed3fbf6dec29983874509b03c4b1c831d70157b6))
* **controls:** release the mouse lock while a menu holds look, and ignore the re-lock warp ([88a34b0](https://github.com/tekulvw/mag-recomp/commit/88a34b09d3ffd1267d4832e58bb46058d93f72d8))
* **installer:** splice the full-width CTR audio command builder by default ([#154](https://github.com/tekulvw/mag-recomp/issues/154)) ([8b7a6aa](https://github.com/tekulvw/mag-recomp/commit/8b7a6aa852dc6a76f8250b741392e906d3c910e1))
* **tools:** payload hook steps update Windows receipt keys in place ([#176](https://github.com/tekulvw/mag-recomp/issues/176)) ([284ebc1](https://github.com/tekulvw/mag-recomp/commit/284ebc16e96433a43c4df890d99a590e41627a0e))


### Performance

* **build:** an x86-64-v4 level, chosen automatically where the CPU has AVX-512 ([#168](https://github.com/tekulvw/mag-recomp/issues/168)) ([95a17ae](https://github.com/tekulvw/mag-recomp/commit/95a17aeefd76308eaa50aaab33bc66dbe884ece9))
* **build:** lift indirectly reached WWS job code (--static-indirect) ([#155](https://github.com/tekulvw/mag-recomp/issues/155)) ([2e6cdf8](https://github.com/tekulvw/mag-recomp/commit/2e6cdf841c34b4cd0d9b5eef2d10894a843f4e2a))
* **build:** register-promote the streamed job modules on every host ([#170](https://github.com/tekulvw/mag-recomp/issues/170)) ([83a3c15](https://github.com/tekulvw/mag-recomp/commit/83a3c156a22750dd847bf73c11df9a9a8aa4c5ab))
* **build:** use x86-64-v3 code generation when the build CPU supports it ([#144](https://github.com/tekulvw/mag-recomp/issues/144)) ([3c40d33](https://github.com/tekulvw/mag-recomp/commit/3c40d338838d826abd3063763d5475596e11b17c))
* **windows:** time the frame clock with the performance counter ([#141](https://github.com/tekulvw/mag-recomp/issues/141)) ([74dae46](https://github.com/tekulvw/mag-recomp/commit/74dae46f37a6f7f575a1ced96542f71503018af9))


### Documentation

* a controller is needed for matches on Windows ([#143](https://github.com/tekulvw/mag-recomp/issues/143)) ([7ec0e49](https://github.com/tekulvw/mag-recomp/commit/7ec0e4988d8137d01a2ad09fed4507d96432cafb))
* add development notes on workflow, the SDK fork, run verification and performance ([#146](https://github.com/tekulvw/mag-recomp/issues/146)) ([a1a091f](https://github.com/tekulvw/mag-recomp/commit/a1a091f2cc103c1d862a9678edda8abf5e394a1a))
* Linux and Windows host build facts, and current installer status ([#145](https://github.com/tekulvw/mag-recomp/issues/145)) ([3c2002b](https://github.com/tekulvw/mag-recomp/commit/3c2002bc6619984f3e5c39ea685105afa8f36ebe))
* list the ps3recomp switches mag-recomp sets and check the list ([#142](https://github.com/tekulvw/mag-recomp/issues/142)) ([5c11668](https://github.com/tekulvw/mag-recomp/commit/5c11668e376c34b20223454a8b12031c70ae56ee))
* record the macOS online match and the first-launch permission step ([#138](https://github.com/tekulvw/mag-recomp/issues/138)) ([f131e36](https://github.com/tekulvw/mag-recomp/commit/f131e367f29c98e9a5815a5eef10b96ffb47a40b))
* **render:** design notes and performance history for the Main path ([#147](https://github.com/tekulvw/mag-recomp/issues/147)) ([60726b5](https://github.com/tekulvw/mag-recomp/commit/60726b553d7b4e0814f9354b2279355d01d61930))
* **render:** Edge geometry job return value and finish contract ([#175](https://github.com/tekulvw/mag-recomp/issues/175)) ([e9f3fe9](https://github.com/tekulvw/mag-recomp/commit/e9f3fe9040a1778ec3061b972e7cfa711a8f1e04))
* **render:** frontend architecture, scope and acceptance ([#148](https://github.com/tekulvw/mag-recomp/issues/148)) ([de2d14d](https://github.com/tekulvw/mag-recomp/commit/de2d14d24c63c6410bb9a740b3ec8144274277bc))
* **render:** specify the Edge geometry child job ([#172](https://github.com/tekulvw/mag-recomp/issues/172)) ([c88968d](https://github.com/tekulvw/mag-recomp/commit/c88968deb424f0dfabfcc7c662b500ef852494de))

## [0.4.0](https://github.com/tekulvw/mag-recomp/compare/v0.3.0...v0.4.0) (2026-11-08)


### Features

* add keyboard mouse and controller binding settings ([#19](https://github.com/tekulvw/mag-recomp/issues/19)) ([1b13665](https://github.com/tekulvw/mag-recomp/commit/1b136651ade047961e8d5bc7358336c61cef2c87))
* add portable Rust input engine and macOS capture ([#16](https://github.com/tekulvw/mag-recomp/issues/16)) ([421c220](https://github.com/tekulvw/mag-recomp/commit/421c220739a78d38a8a3558ba8a9a0882e198fbe))
* **boot:** turn on movie playback for engines built with FFmpeg ([#104](https://github.com/tekulvw/mag-recomp/issues/104)) ([9616fb1](https://github.com/tekulvw/mag-recomp/commit/9616fb18cdb5d67f89858bac99a07a6c2d2f3955))
* **diagnostics:** opt-in watchdog for stalled job-completion waits ([#113](https://github.com/tekulvw/mag-recomp/issues/113)) ([2f44452](https://github.com/tekulvw/mag-recomp/commit/2f44452ff06d5afa1a3b0df024c023b26843a4cd))
* **installer:** ask Windows players for their name on first launch ([#132](https://github.com/tekulvw/mag-recomp/issues/132)) ([e639bd5](https://github.com/tekulvw/mag-recomp/commit/e639bd5217eb8ab2647e950fb3b419c252fd7031))
* **installer:** build the Windows player on the runtime's Vulkan backend ([#128](https://github.com/tekulvw/mag-recomp/issues/128)) ([075e0f9](https://github.com/tekulvw/mag-recomp/commit/075e0f9ee71a2095c8ccb3a7b394c89948e3b566))
* let several extensions register lifecycle callbacks ([0e4ddbc](https://github.com/tekulvw/mag-recomp/commit/0e4ddbc995a39978e87d2f67e131ba000f2da2b9))
* **player:** server and player name settings on Windows and Linux ([#125](https://github.com/tekulvw/mag-recomp/issues/125)) ([e38c6a5](https://github.com/tekulvw/mag-recomp/commit/e38c6a5fbbef26d6c80f87f4962f8617ed2bd4ea))
* **render:** adapt a captured Shadow run to the native cascade pass ([b3e22c2](https://github.com/tekulvw/mag-recomp/commit/b3e22c2792bd1ed4141b17ff8a7f3506d36b81c8))
* **render:** add the portable render adapter core crate ([#27](https://github.com/tekulvw/mag-recomp/issues/27)) ([95d6d8c](https://github.com/tekulvw/mag-recomp/commit/95d6d8ca03f49e6e8134a51db826a229139989e9))
* **render:** animated fragment built-in rows from decoded role values in the glue ([caadf21](https://github.com/tekulvw/mag-recomp/commit/caadf215efef194fdf079f5ff9da045c3558701c))
* **render:** apply clean-room spec errata and add group 0 and group 3 wind ([#44](https://github.com/tekulvw/mag-recomp/issues/44)) ([177aead](https://github.com/tekulvw/mag-recomp/commit/177aead792f5593439136a4831852448d910211c))
* **render:** clean-room group transitions and Main producer effects ([e9ec299](https://github.com/tekulvw/mag-recomp/commit/e9ec2998c3c03c396279874e4e3a4c5883fce561))
* **render:** clean-room group transitions and Main producer effects ([c985f0a](https://github.com/tekulvw/mag-recomp/commit/c985f0abce928351c4fb89692bc23cddc9b44986))
* **render:** clean-room Main lighting, group-twelve and fog producers ([d02129d](https://github.com/tekulvw/mag-recomp/commit/d02129d96b305e61a490d2d39f1f850d82f6cc72))
* **render:** clean-room Main lighting, group-twelve and fog producers ([d058dae](https://github.com/tekulvw/mag-recomp/commit/d058dae5684f6043d88fd25975f7b3d00d7945d9))
* **render:** clean-room Main scene planner core ([87129f2](https://github.com/tekulvw/mag-recomp/commit/87129f23e4e540271d5e1bd433d147f270b60dd3))
* **render:** clean-room Main scene planner core ([6bf07dd](https://github.com/tekulvw/mag-recomp/commit/6bf07dd1bc7523c7c502edfded2271d8f4db11dd))
* **render:** clean-room material values, program compilation, vertex inputs and raster state ([705b40a](https://github.com/tekulvw/mag-recomp/commit/705b40a6f927c4e0b48dda999230eda640d7bc6e))
* **render:** clean-room material values, program compilation, vertex inputs and raster state ([38deb26](https://github.com/tekulvw/mag-recomp/commit/38deb269bfe8c2ed8391429f01de846641a3f40c))
* **render:** clean-room packed normals, skinning and ripple deformation ([3084cec](https://github.com/tekulvw/mag-recomp/commit/3084cecdb2e2ed5663523425730b2c17baf4c016))
* **render:** clean-room packed normals, skinning and ripple deformation ([e1ddae0](https://github.com/tekulvw/mag-recomp/commit/e1ddae04b01e1903abf7c0ba817216098e221bc4))
* **render:** clean-room partition streams, material sources and fragment built-ins ([#50](https://github.com/tekulvw/mag-recomp/issues/50)) ([8b7729b](https://github.com/tekulvw/mag-recomp/commit/8b7729b78e386604e6bf3bf4267f645f9ab905f6))
* **render:** clean-room Shadow pass behaviour, settled parts only ([#58](https://github.com/tekulvw/mag-recomp/issues/58)) ([0356886](https://github.com/tekulvw/mag-recomp/commit/035688633736b37a24e3ea55a401ff1e4d893711))
* **render:** clean-room texture unit controls and material setup ([de80cb5](https://github.com/tekulvw/mag-recomp/commit/de80cb5c5873e60ee418848bbe04a7879e1bc7b4))
* **render:** clean-room texture unit controls and material setup ([453a2c0](https://github.com/tekulvw/mag-recomp/commit/453a2c054c792d4d07a06cf60d6240da8750320a))
* **render:** cohort and update-loop bridges for the render adapter ([#76](https://github.com/tekulvw/mag-recomp/issues/76)) ([015609d](https://github.com/tekulvw/mag-recomp/commit/015609da4a783b830aac21955c2710ee3432cefb))
* **render:** compute animated fragment built-in rows with the clean-room scale factors ([f2e9be2](https://github.com/tekulvw/mag-recomp/commit/f2e9be2da66110f06e841c86b5d0c5bd46101e79))
* **render:** deform group 3 and 4 Shadow casters as Main does ([0ebe053](https://github.com/tekulvw/mag-recomp/commit/0ebe053e1f046ccf997d2804c331048df60a05ad))
* **render:** describe the native Shadow cascade pass from the captured facts ([966cd06](https://github.com/tekulvw/mag-recomp/commit/966cd0642cdb6cdafd6b3435921df454adba8573))
* **render:** glue between the clean-room behaviour crate and the adapter core ([dd20fcf](https://github.com/tekulvw/mag-recomp/commit/dd20fcf1f53a7d6d6d8f6ca6cfcdd689a1914112))
* **render:** glue between the clean-room behaviour crate and the adapter core ([e5aa160](https://github.com/tekulvw/mag-recomp/commit/e5aa1603fe1809f01d36306ade30a410a0e13857))
* **render:** implement the Shadow pass facts settled by the job recording ([5f29743](https://github.com/tekulvw/mag-recomp/commit/5f297432ac05673f9b70b930d8cf03f2905a7eb9))
* **render:** migrate adapter cohort geometry and depth sources ([7329f62](https://github.com/tekulvw/mag-recomp/commit/7329f622bd231ed8d136301883083a1938db6dea))
* **render:** migrate adapter Main finish, group-twelve inputs and client scene ([14a2ad0](https://github.com/tekulvw/mag-recomp/commit/14a2ad0972f64ebd0f51d716b433fb99d6eacb8e))
* **render:** migrate adapter source capture, caches and stream codecs ([c029d45](https://github.com/tekulvw/mag-recomp/commit/c029d454a9c72f39bd9832e7b77b65b838af7d34))
* **render:** migrate the adapter cohort, source-copy and client scene entry points ([3766990](https://github.com/tekulvw/mag-recomp/commit/376699089bf1a763ecff70911afc24405de116bf))
* **render:** migrate the adapter depth source entry point ([88f3d42](https://github.com/tekulvw/mag-recomp/commit/88f3d423bb073229e5c1e20572d9ff0b58989d0c))
* **render:** migrate the adapter material capture, values, compilation and vertex inputs ([569c5ed](https://github.com/tekulvw/mag-recomp/commit/569c5ed44790f719855e259f6fa1311ee2807151))
* **render:** migrate the ring terminal writers and generalise the empty pass ([#64](https://github.com/tekulvw/mag-recomp/issues/64)) ([4092cf9](https://github.com/tekulvw/mag-recomp/commit/4092cf97dc6f0bbb31754e73311a146f6739175e))
* **render:** native depth bias (constant plus slope, no clamp) ([#54](https://github.com/tekulvw/mag-recomp/issues/54)) ([9e0aa82](https://github.com/tekulvw/mag-recomp/commit/9e0aa82a78be7d75ad90f84e199ad000111fd67e))
* **render:** native GPU timing on Vulkan ([#55](https://github.com/tekulvw/mag-recomp/issues/55)) ([3469bee](https://github.com/tekulvw/mag-recomp/commit/3469beeb2cdd724bca4387579377e0a4fb1d3504))
* **render:** opt-in deformation sources in the Depth source capture ([5c66ac2](https://github.com/tekulvw/mag-recomp/commit/5c66ac2d4aaa66339979d0443b5d36f8581e7e92))
* **render:** opt-in GPU group-zero deformation through the packet compute prefix ([053922d](https://github.com/tekulvw/mag-recomp/commit/053922d539811f7bbccd336ba1a8274a20310d97))
* **render:** opt-in render adapter build from the public crates ([b128f89](https://github.com/tekulvw/mag-recomp/commit/b128f891079adbc7877db66e25ee5ef6822b7118))
* **render:** opt-in reserved-order Main commit on Vulkan ([#23](https://github.com/tekulvw/mag-recomp/issues/23)) ([ba0531c](https://github.com/tekulvw/mag-recomp/commit/ba0531c0f619f7707799024381b7b27d320023b4))
* **render:** opt-in resident uploaded images on Vulkan ([#26](https://github.com/tekulvw/mag-recomp/issues/26)) ([de496cf](https://github.com/tekulvw/mag-recomp/commit/de496cf453d9ff7124bb54dc55ffd9fa1d7a9575))
* **render:** opt-in Vulkan host and native frontend on Windows ([#124](https://github.com/tekulvw/mag-recomp/issues/124)) ([270886a](https://github.com/tekulvw/mag-recomp/commit/270886a67373d0a77a1cc2ca16dbb62a71336f1d))
* **render:** owned-arena builder for the scene packet ABI ([#45](https://github.com/tekulvw/mag-recomp/issues/45)) ([b98b6c9](https://github.com/tekulvw/mag-recomp/commit/b98b6c9faaa838dc784c574f57000b428b917c65))
* **render:** position reservations that bind their imports at the boundary ([#106](https://github.com/tekulvw/mag-recomp/issues/106)) ([37bdc6f](https://github.com/tekulvw/mag-recomp/commit/37bdc6f4706d3784b91a3da678dc684915493e02))
* **render:** position-only reservations through the native worker ([6591890](https://github.com/tekulvw/mag-recomp/commit/659189096335745bdeae759d53af801a5d7cdca7))
* **render:** position-only scene reservations on Metal ([d5d6c53](https://github.com/tekulvw/mag-recomp/commit/d5d6c539fa93818a95541bd3d474c2326519a3b5))
* **render:** publish adapter RSX state, program and stream readers ([97ed40a](https://github.com/tekulvw/mag-recomp/commit/97ed40adacbcd823ba381e18e94f25de27c6e327))
* **render:** publish adapter RSX state, program and stream readers ([5ef6577](https://github.com/tekulvw/mag-recomp/commit/5ef65776d9a5255f8282db3b2fca42338f40a1f1))
* **render:** publish adapter Shadow, cohort and binding readers ([2cdf4f9](https://github.com/tekulvw/mag-recomp/commit/2cdf4f92d33bdb21134f8c0cc35949f57911a30e))
* **render:** publish adapter Shadow, cohort and binding readers ([e2c6f7a](https://github.com/tekulvw/mag-recomp/commit/e2c6f7ae8dd98bddc2ccdf3846c32dacc885d828))
* **render:** skin admitted group 7 Shadow casters as Main does ([24d001c](https://github.com/tekulvw/mag-recomp/commit/24d001cd4c6c3ab9bc9f4f88b8b28e684f1a53ca))
* **render:** stage portable Rust frontend integration ([#21](https://github.com/tekulvw/mag-recomp/issues/21)) ([9cf89d6](https://github.com/tekulvw/mag-recomp/commit/9cf89d6c6f00ea5d96c4d213ae24851d65fc1d9a))
* **render:** state-only replay of recorded RSX method writes ([04d68ed](https://github.com/tekulvw/mag-recomp/commit/04d68ed26d3a93f0d3b190ffd6d177388026372b))
* **render:** stored-depth bounds on D3D12 through the depth bounds test ([#111](https://github.com/tekulvw/mag-recomp/issues/111)) ([7243293](https://github.com/tekulvw/mag-recomp/commit/72432938bdc75bf5dec1eb1f9a5169583417ab85))
* **render:** stored-depth bounds on Vulkan through the depth bounds test ([#24](https://github.com/tekulvw/mag-recomp/issues/24)) ([ca2d015](https://github.com/tekulvw/mag-recomp/commit/ca2d015fc0f3a378cda68acfa0572fef31d2dd42))
* support shared native button bindings across menu contexts ([#20](https://github.com/tekulvw/mag-recomp/issues/20)) ([66e9204](https://github.com/tekulvw/mag-recomp/commit/66e92041392a2776ab23b5770b5979cfa39b0ecf))
* **tools:** discover indirect-branch targets in WWS job modules ([#57](https://github.com/tekulvw/mag-recomp/issues/57)) ([817efac](https://github.com/tekulvw/mag-recomp/commit/817efac85c310aa1e854e0ab5ddc7c4177725bdf))
* **tools:** pinned render adapter observer hooks for lifted PPU sources ([c52cb4f](https://github.com/tekulvw/mag-recomp/commit/c52cb4fc4af14abf6995b444f2934e6f36afd7c8))


### Bug Fixes

* **build:** compile SPU program sources from the checked directory ([#116](https://github.com/tekulvw/mag-recomp/issues/116)) ([28ce214](https://github.com/tekulvw/mag-recomp/commit/28ce21495b22191ed5c67093ab175a6872ec8735))
* **build:** keep the SPU decode cache off the thread stack on Linux ([#134](https://github.com/tekulvw/mag-recomp/issues/134)) ([af9e28c](https://github.com/tekulvw/mag-recomp/commit/af9e28c58731eac5057e6eef99b9eff899bf9470))
* **engine:** use a platform-neutral default window title ([#137](https://github.com/tekulvw/mag-recomp/issues/137)) ([0e0fbb4](https://github.com/tekulvw/mag-recomp/commit/0e0fbb49df9b95008e487aa3e5ae3cc2d3ba74d1))
* **installer:** keep a failed attempt's output and retry past bytecode caches ([#121](https://github.com/tekulvw/mag-recomp/issues/121)) ([7c31bb7](https://github.com/tekulvw/mag-recomp/commit/7c31bb7e9d66d399f3fec48da1a0955ce107d378))
* **installer:** keep Linux compiler driver names when resolving the recipe ([#129](https://github.com/tekulvw/mag-recomp/issues/129)) ([67ca600](https://github.com/tekulvw/mag-recomp/commit/67ca6003a96d087f699926b5ac887297fc962ad6))
* **installer:** keep players' saves in the installation ([#135](https://github.com/tekulvw/mag-recomp/issues/135)) ([6e5b8da](https://github.com/tekulvw/mag-recomp/commit/6e5b8dad3b0fba68c6f3284d5b44e07dcd79ed72))
* **installer:** keep the build interpreter's library path for PyInstaller ([#96](https://github.com/tekulvw/mag-recomp/issues/96)) ([80636ff](https://github.com/tekulvw/mag-recomp/commit/80636ffd9f748374cc68345760f16cf13249278b))
* **installer:** keep the reviewed dispatcher body identity ([b0ab3b4](https://github.com/tekulvw/mag-recomp/commit/b0ab3b42334500442ef458c661749c24db86c676))
* **installer:** pin and pack the dispatcher splice's fixed lifter ([#120](https://github.com/tekulvw/mag-recomp/issues/120)) ([953b5f1](https://github.com/tekulvw/mag-recomp/commit/953b5f167fc97e4d91af180f3562443ea252c7b0))
* **installer:** refuse a changed settings archive and never hang on it at launch ([#123](https://github.com/tekulvw/mag-recomp/issues/123)) ([0db9326](https://github.com/tekulvw/mag-recomp/commit/0db932603fcf54fd8ed8fedc086e0d31d6ca4bcc))
* **installer:** ship the movie playback defaults in packaged players ([#131](https://github.com/tekulvw/mag-recomp/issues/131)) ([ef03133](https://github.com/tekulvw/mag-recomp/commit/ef03133140f2970d1c1c75ffbc7ec60e14468d26))
* **installer:** splice the dispatcher with the tagged lifter fix ([#82](https://github.com/tekulvw/mag-recomp/issues/82)) ([2ab94ce](https://github.com/tekulvw/mag-recomp/commit/2ab94cea0f9302ce2878f6cdd515318b5d5f3176))
* **installer:** stage the pinned ELF parser for the dispatcher splice ([d5b7267](https://github.com/tekulvw/mag-recomp/commit/d5b7267a9d5c386a32dada559eca1a1b3c59789c))
* **installer:** survive GitHub release outages and explain download failures ([#126](https://github.com/tekulvw/mag-recomp/issues/126)) ([bc07a10](https://github.com/tekulvw/mag-recomp/commit/bc07a1005540ac144761a8e0b070d4635d163c2c))
* **installer:** trust the system CA bundle for downloads on macOS ([#122](https://github.com/tekulvw/mag-recomp/issues/122)) ([7324f39](https://github.com/tekulvw/mag-recomp/commit/7324f39ce3401d85e78c5f9ab80a76231dbfdc29))
* **player:** present each executed guest flip once ([#90](https://github.com/tekulvw/mag-recomp/issues/90)) ([59231c2](https://github.com/tekulvw/mag-recomp/commit/59231c248faf11749e55c50b50f76d1f3a1392d6))
* **player:** sign in to the server with the player's name ([#130](https://github.com/tekulvw/mag-recomp/issues/130)) ([7b740b3](https://github.com/tekulvw/mag-recomp/commit/7b740b359ab1739443a9c1b48bf1a841f61eb270))
* **render:** a positioned commit never waits on work behind its position, and a declined one completes its ticket ([#108](https://github.com/tekulvw/mag-recomp/issues/108)) ([f2a81cb](https://github.com/tekulvw/mag-recomp/commit/f2a81cbbb84059dbc565a0a1a8c229680e46d506))
* **render:** admit packets whose producer declares unresolved items ([#48](https://github.com/tekulvw/mag-recomp/issues/48)) ([a89afb2](https://github.com/tekulvw/mag-recomp/commit/a89afb29d4149f75486bfd28d2ddc9362003a315))
* **render:** attribute Vulkan scene refusals and check anisotropic samplers ([#51](https://github.com/tekulvw/mag-recomp/issues/51)) ([e3f9629](https://github.com/tekulvw/mag-recomp/commit/e3f96296dc0f807e77843bf6bbd8c811238e6eb9))
* **render:** build the native frontend on Windows with this build's compilers ([#102](https://github.com/tekulvw/mag-recomp/issues/102)) ([11f6b48](https://github.com/tekulvw/mag-recomp/commit/11f6b484ec91851e6b8be07e9d096dd9be56d086))
* **render:** check every technique-chunk region pair and cover the settled readings ([#53](https://github.com/tekulvw/mag-recomp/issues/53)) ([1a5a83e](https://github.com/tekulvw/mag-recomp/commit/1a5a83e310626c80e94eaec3e7a226c3b2ffe489))
* **render:** classify context DMA bindings in the RSX state preamble ([e21ed71](https://github.com/tekulvw/mag-recomp/commit/e21ed717bd630d85831b0862adb43d54dfec4bd7))
* **render:** classify the whole NV4097 method table in the state preamble ([72b7534](https://github.com/tekulvw/mag-recomp/commit/72b75341dbfcc619c04eaa903add08fc1fbc6a51))
* **render:** compile owned legacy HLSL on D3D12 with HLSL 2018 semantics ([#114](https://github.com/tekulvw/mag-recomp/issues/114)) ([ae122a2](https://github.com/tekulvw/mag-recomp/commit/ae122a2976b0cb226e42d697c8ddd57519f2ed63))
* **render:** compile the translator's HLSL on D3D12 despite -WX ([c42825d](https://github.com/tekulvw/mag-recomp/commit/c42825d68255e6ca0d70e02d67d4b46e568e471b))
* **render:** keep Rust thread stacks usable beside the runtime's static TLS ([#46](https://github.com/tekulvw/mag-recomp/issues/46)) ([9252afc](https://github.com/tekulvw/mag-recomp/commit/9252afc95dafda82c751f23258fe42fca6b472d8))
* **render:** keep the ring generation in bits 30-31 of the epoch word ([#81](https://github.com/tekulvw/mag-recomp/issues/81)) ([4e93b09](https://github.com/tekulvw/mag-recomp/commit/4e93b09c4c558cdf5f37a9a34420bf9724109bdf))
* **render:** link versioned shared glslang libraries on Linux ([#117](https://github.com/tekulvw/mag-recomp/issues/117)) ([c4250fd](https://github.com/tekulvw/mag-recomp/commit/c4250fde374b562ff2472f1993604cb4b68f3afe))
* **render:** never refuse Main admission because the host is slow to poll ([#22](https://github.com/tekulvw/mag-recomp/issues/22)) ([2c2950a](https://github.com/tekulvw/mag-recomp/commit/2c2950ae10f795f4fa4ae2248608049e78e5828a))
* **render:** positioned commits work across threads: no re-pin, release order, capacity backpressure, refusal logging and a cross-thread harness ([#110](https://github.com/tekulvw/mag-recomp/issues/110)) ([aa03c75](https://github.com/tekulvw/mag-recomp/commit/aa03c7531e0c79494bbd84b90f71440d2b784a9f))
* **render:** read a Shadow run's own family lists in the Depth source capture ([#85](https://github.com/tekulvw/mag-recomp/issues/85)) ([4502fe9](https://github.com/tekulvw/mag-recomp/commit/4502fe91c5e4b5fc8b0bb63302f7ed8e499931d3))
* **render:** read the enabled-group mask from the low half of the group-enable word ([#70](https://github.com/tekulvw/mag-recomp/issues/70)) ([f4dd443](https://github.com/tekulvw/mag-recomp/commit/f4dd443a0c1ccfac9cea3266337d8588295474cd))
* **render:** read the Main group-enable mask from the low half of its word ([0f27a13](https://github.com/tekulvw/mag-recomp/commit/0f27a13ee21a3e03b19dbcb90f5f7f03af031738))
* **render:** refuse D3D12 batch commands that name buffers outside the batch ([fe57d65](https://github.com/tekulvw/mag-recomp/commit/fe57d65f713fa8ea64486947fb5bc7203f07d6a1))
* **render:** releasing a position reservation waits for the caller lock ([#107](https://github.com/tekulvw/mag-recomp/issues/107)) ([cd988f5](https://github.com/tekulvw/mag-recomp/commit/cd988f5906077d5ce882ee45e7bd25ebbe0ac321))
* **render:** size D3D12 attachment view heaps for scene image indices ([#115](https://github.com/tekulvw/mag-recomp/issues/115)) ([985f5b9](https://github.com/tekulvw/mag-recomp/commit/985f5b9cdcd21eb4d3334a9bb25291fa231480d2))
* **runtime:** map the POSIX guest arena page and 4 GB aligned ([#119](https://github.com/tekulvw/mag-recomp/issues/119)) ([5346776](https://github.com/tekulvw/mag-recomp/commit/534677625ddc924084445501a75d5b704d8eeb11))
* **tools:** run the Metal native checker in the inherited environment ([#109](https://github.com/tekulvw/mag-recomp/issues/109)) ([e51c316](https://github.com/tekulvw/mag-recomp/commit/e51c316c1703880a070e79fc78aa28d4496f4988))
* **tools:** validate generated modules lifted on another host's path separator ([#112](https://github.com/tekulvw/mag-recomp/issues/112)) ([e69ebaa](https://github.com/tekulvw/mag-recomp/commit/e69ebaa68bbf92b83a0fd78cbf727692aa1bb09c))


### Performance

* **render:** fixed-size keys for the native sampler cache ([f461269](https://github.com/tekulvw/mag-recomp/commit/f461269fda5f34d40e13f18fc4309149ce31ca31))
* **render:** opt-in packet handover and worker-only validation for Main ([#84](https://github.com/tekulvw/mag-recomp/issues/84)) ([f8419df](https://github.com/tekulvw/mag-recomp/commit/f8419df497026ebf5604a94a749cc074c788fc09))
* **render:** reuse the native scene submission arrays ([abc16b2](https://github.com/tekulvw/mag-recomp/commit/abc16b24375365d1049e65496640266aa251b2ed))
* ship the accepted runtime defaults on every host ([#136](https://github.com/tekulvw/mag-recomp/issues/136)) ([1986be9](https://github.com/tekulvw/mag-recomp/commit/1986be976cf53b0ed6cf60b0030b002f78435ffa))


### Documentation

* a "For players" section in the README ([#127](https://github.com/tekulvw/mag-recomp/issues/127)) ([09d9a5c](https://github.com/tekulvw/mag-recomp/commit/09d9a5c64dda5458ccc59d2b6747e4ed22c1db4e))
* allow function hashes as identities in the clean-room method ([ba7549a](https://github.com/tekulvw/mag-recomp/commit/ba7549a4687a76192ea08856603cc7bdcf88e3dd))
* let default-off in-repo code use the extension points ([#79](https://github.com/tekulvw/mag-recomp/issues/79)) ([e85a394](https://github.com/tekulvw/mag-recomp/commit/e85a3940b047fb339a1a87aed2d68a3fb6a8a2e2))
* **render:** clean-room behaviour specification for the shadow pass ([#56](https://github.com/tekulvw/mag-recomp/issues/56)) ([e471f90](https://github.com/tekulvw/mag-recomp/commit/e471f9007a015b2443c1fd705d24fc3a8163c62c))
* **render:** clean-room behaviour specifications for deformation ([e3025f9](https://github.com/tekulvw/mag-recomp/commit/e3025f91b4bbf8c86ab78dc3703481ea048709d5))
* **render:** clean-room behaviour specifications for deformation ([6f23d17](https://github.com/tekulvw/mag-recomp/commit/6f23d17c55fc214ddcbcb9a80de103da86b892a5))
* **render:** clean-room behaviour specifications for Main planning and setup ([e082691](https://github.com/tekulvw/mag-recomp/commit/e082691522d7b0d13a432a97f215c6582ec78612))
* **render:** clean-room behaviour specifications for Main planning and setup ([3fb39c2](https://github.com/tekulvw/mag-recomp/commit/3fb39c28dfc4d3e044ec3d975ffdd4b335167104))
* **render:** clean-room behaviour specifications for Main producers ([981127e](https://github.com/tekulvw/mag-recomp/commit/981127e26164b3a7b798c5bdd6342d18db7e183b))
* **render:** clean-room behaviour specifications for Main producers ([d384dfb](https://github.com/tekulvw/mag-recomp/commit/d384dfbb893c32a1f4ea9a9a85ca537088ccc4d9))
* **render:** clean-room specifications for Main planner dependencies ([3989fb9](https://github.com/tekulvw/mag-recomp/commit/3989fb996a316af44d1e7b892d52f8a2d6e37cf7))
* **render:** clean-room specifications for Main planner dependencies ([c6d91d6](https://github.com/tekulvw/mag-recomp/commit/c6d91d67551ec44b1bc528856e4d6c59e03e06cc))
* **render:** close the synthetic Main frame's specification gaps ([#49](https://github.com/tekulvw/mag-recomp/issues/49)) ([3621060](https://github.com/tekulvw/mag-recomp/commit/3621060f352fe45f97a791775399012f034e63b9))
* **render:** integration design for the clean-room behaviour crate ([a748446](https://github.com/tekulvw/mag-recomp/commit/a748446367f362a73d7b9c792fe676489af8b062))
* **render:** integration design for the clean-room behaviour crate ([10915f7](https://github.com/tekulvw/mag-recomp/commit/10915f7b3294eafc92a09d42f08525b25923bd10))
* **render:** resolve implementer errata in clean-room specifications ([2aa89f7](https://github.com/tekulvw/mag-recomp/commit/2aa89f7026b65c7ae5af41a8d61f31343d24cb2d))
* **render:** resolve implementer errata in clean-room specifications ([64f299d](https://github.com/tekulvw/mag-recomp/commit/64f299d73b6cc5adb96b5adbfa8f81cad82d3323))
* **render:** settle the implementer's stream, built-in and chunk readings ([#52](https://github.com/tekulvw/mag-recomp/issues/52)) ([58f38d5](https://github.com/tekulvw/mag-recomp/commit/58f38d5651c03916463feae14d7411d1da0a1031))
* **render:** settle the shadow pass from its job-IO recording ([#59](https://github.com/tekulvw/mag-recomp/issues/59)) ([35cbbe6](https://github.com/tekulvw/mag-recomp/commit/35cbbe6a5ce9d42b83e2eac9169f3430e922459c))
* **render:** settle the shadow surface, raster state and unit-1 link ([#75](https://github.com/tekulvw/mag-recomp/issues/75)) ([bac3df4](https://github.com/tekulvw/mag-recomp/commit/bac3df432acdba52c2890096b89ba076c0c65e4f))
* **render:** shadow ring rule, shadow surface facts and the group-mask erratum ([#68](https://github.com/tekulvw/mag-recomp/issues/68)) ([41f9a6a](https://github.com/tekulvw/mag-recomp/commit/41f9a6a3799df97cb1729f7383160fd68fe4b7b0))

## [0.3.0](https://github.com/tekulvw/mag-recomp/compare/v0.2.0...v0.3.0) (2026-11-01)


### Features

* add independent internal rendering resolution ([#12](https://github.com/tekulvw/mag-recomp/issues/12)) ([8c3dae8](https://github.com/tekulvw/mag-recomp/commit/8c3dae8031ac8d7bb01a2a9c58d11d59f8aead16))
* add native macOS player settings and community server defaults ([#11](https://github.com/tekulvw/mag-recomp/issues/11)) ([3541831](https://github.com/tekulvw/mag-recomp/commit/354183110b4594675cfab96025a72ace13c4e612))


### Bug Fixes

* restore compiled SPU jobs and reproducible rendering defaults ([#15](https://github.com/tekulvw/mag-recomp/issues/15)) ([303fd1d](https://github.com/tekulvw/mag-recomp/commit/303fd1d1518aa4d9bfab43e33824d2424371e545))

## [0.2.0](https://github.com/tekulvw/mag-recomp/compare/v0.1.0...v0.2.0) (2026-10-28)


### Features

* add build extension points (workspace, pin overlay, CMake, receipts, lift hooks, runtime callbacks) ([c105a40](https://github.com/tekulvw/mag-recomp/commit/c105a40a09509575549ec982de131f4c6a33231c))
* **build:** include MAG_EXTENSIONS and resolve per-machine profiles against MAG_WORKSPACE ([4a4fd38](https://github.com/tekulvw/mag-recomp/commit/4a4fd381497a8e06201c707ce289b62499a40bd1))
* **installer:** validate early, recover failed builds and support Linux ([f988851](https://github.com/tekulvw/mag-recomp/commit/f9888516b23540217d49bda09bab92a73dcf92f4))
* **installer:** validate early, recover failed builds and support Linux ([47e7b24](https://github.com/tekulvw/mag-recomp/commit/47e7b2413f3345f8ebea109839673100d2d309fb))
* **lift:** apply extension hook modules after the built-in hooks ([628a144](https://github.com/tekulvw/mag-recomp/commit/628a144732516e8fab391bc6bcce6a4091791154))
* **provenance:** record extension inputs and the framework file in build receipts ([336b498](https://github.com/tekulvw/mag-recomp/commit/336b49821e8638dd05550148969096c35c6df708))
* **runtime:** add an extension callback registry for start, frame, trace and payload hooks ([876f2aa](https://github.com/tekulvw/mag-recomp/commit/876f2aa42931abcf1c71e140b3d34ae2f1506db4))
* **tools:** resolve working data against MAG_WORKSPACE and allow a workspace pin overlay ([7a6395a](https://github.com/tekulvw/mag-recomp/commit/7a6395afbe785f01cc9f22f296a74a4fcda56b19))


### Bug Fixes

* extension point follow-ups from review ([8a4b7b6](https://github.com/tekulvw/mag-recomp/commit/8a4b7b61f973c4fdaeaf45d1cc08ab45a9a52d1c))
* **installer:** compile Linux runtime and restore dispatcher build fix ([e9bdf51](https://github.com/tekulvw/mag-recomp/commit/e9bdf519833631011d5b3bcb43f84bade9df6ad1))
* **installer:** keep inherited overrides out of Windows builds and launches ([8ad13dc](https://github.com/tekulvw/mag-recomp/commit/8ad13dc0a6f742b8d0683682847be556537bea06))
* **installer:** keep inherited overrides out of Windows builds and launches ([06aae4d](https://github.com/tekulvw/mag-recomp/commit/06aae4dff800b221eca0dbd46df9bd17e6a690bf))
* **installer:** preserve case-distinct Linux tool archive files ([2550ed6](https://github.com/tekulvw/mag-recomp/commit/2550ed6857d993ece4887200a6e34daff375befd))
* **installer:** verify each installer's own kit so releases can be attached ([fd70290](https://github.com/tekulvw/mag-recomp/commit/fd702905dee31b28b9e753e6eb4b5120aab6570a))
* **installer:** verify each installer's own kit so releases can be attached ([03f2d8d](https://github.com/tekulvw/mag-recomp/commit/03f2d8da5a549fe3f0fb60613427355433eafbb8))
* keep extension callbacks inside errno protection and record only applied hooks ([1a45ac5](https://github.com/tekulvw/mag-recomp/commit/1a45ac5f5f27309e3e19f4da6dcb8fc41ddf9141))


### Documentation

* add recomp.fyi manifest ([030c198](https://github.com/tekulvw/mag-recomp/commit/030c1985350bddac8c241e72cd4952d19c0efe67))
* add recomp.fyi manifest ([cc6d223](https://github.com/tekulvw/mag-recomp/commit/cc6d223c9f687509a2c21c465648b7570cb13613))
* describe the six build extension points ([c8a3d3e](https://github.com/tekulvw/mag-recomp/commit/c8a3d3ee7eee831e5417e99947ac95f20cb593a0))
* list Windows and Linux as recomp.fyi targets ([115278f](https://github.com/tekulvw/mag-recomp/commit/115278fec2a2a68fd4b8631cad79e6ddfad3e512))

## 0.1.0 (2026-10-27)


### Features

* **installer:** build and attach macOS and Windows installers to releases ([34bc324](https://github.com/tekulvw/mag-recomp/commit/34bc324a5e99b8314b96a056adc9dd24bb4e3008))
* **installer:** one-button installers for macOS and Windows ([53b02b6](https://github.com/tekulvw/mag-recomp/commit/53b02b6b7fb951b9d4f1c910b4776f42f3d7d29f))


### Bug Fixes

* **deps:** pin ps3recomp by verified tags and accept only the pinned runtime ([941dbfa](https://github.com/tekulvw/mag-recomp/commit/941dbfaea1e478777096fc5dafcb8094fd9e8c6c))
* **installer:** refuse untracked files when preparing a kit from a checkout ([a7aa4da](https://github.com/tekulvw/mag-recomp/commit/a7aa4da58b35ad8e650702152ca1bf6b85567940))
* pin the published ps3recomp runtime and document the SPU programs lift ([63c3f6f](https://github.com/tekulvw/mag-recomp/commit/63c3f6f94a7962127a2ac891a13402406d7c3377))
* pin two graphics hook bodies to a fresh lift ([551dfea](https://github.com/tekulvw/mag-recomp/commit/551dfea6a2dbb24238d2ede9182d6120e89c5229))
* re-pin profiles changed by the lifter path change ([fef93dc](https://github.com/tekulvw/mag-recomp/commit/fef93dcfe013c12413f1e5ccf424cdb8516217e9))
* **windows:** build MAG's sources and the pinned runtime with clang-cl ([72882d2](https://github.com/tekulvw/mag-recomp/commit/72882d294dad4b5f489c5bc993672f5baf25cea9))


### Refactoring

* drop the experimental native renderer and its diagnostics ([ec784de](https://github.com/tekulvw/mag-recomp/commit/ec784def957c240b924d29217c95d1095ea710fe))
* rename builds/ to profiles/ ([73f6648](https://github.com/tekulvw/mag-recomp/commit/73f66486cb8d7811f9e408e552f9dd4ed1dff4f9))


### Documentation

* license the project under MIT ([6a59258](https://github.com/tekulvw/mag-recomp/commit/6a5925836176473cc9f12f28a4e3d84a0bed89f0))
* release assets are the installers built from the release's source ([c17c246](https://github.com/tekulvw/mag-recomp/commit/c17c246451d2e67293a5567bcb36ac1b87925c3d))
