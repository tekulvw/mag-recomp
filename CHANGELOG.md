# Changelog

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
