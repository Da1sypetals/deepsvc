# DeepSVC ARA Plugin

ARA plugin for singing-voice conversion algorithms. Currently, this plugin is heavily tailored for Yingmusic-SVC, and will remain so until the next state-of-the-art algorithm (in the sense of output quality) is invented.

Supports VST3 and AudioUnit; works only on Apple Silicon because of the dependency of Metal.


## Acknowledgements

- [OpenTune](https://github.com/YuFeng926/OpenTune) for inspiration and project architecture
- [Diffsinger community vocoders](https://openvpi.github.io/vocoders/) and [Pupu-Vocoder](https://nsfpupuvocoder.github.io/) for vocoders
- [YingMusic-SVC](https://github.com/GiantAILab/YingMusic-SVC) for SVC algorithm
- [mlx](https://github.com/ml-explore/mlx) and [mlx-rs](https://github.com/oxiglade/mlx-rs) for hardware acceleration

