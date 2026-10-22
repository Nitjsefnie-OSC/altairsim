# disks/

This directory holds disk images for the simulated disk controllers, with the notes and the
source that explain each image. It is a working collection for someone who has the repository.
It is not part of the release package.

Some acceptance tests read files from this directory. If you move or rename a file here,
search [`tests/acceptance/`](../tests/acceptance/) for its path first.

## How it is arranged

The first level is the disk controller. The second level is the operating system. Below that,
each folder is one disk set and has its own `README.md`.

```
disks/<controller>/<operating system>/<disk set>/
```

| Folder | Controller | What is below it |
|---|---|---|
| `mits-88dcdd/` | MITS 88-DCDD, the 8-inch floppy controller | `cpm22/` has several CP/M 2.2 disk sets. `diskbasic/` is for Altair Disk BASIC. |
| `mits-88mds/` | MITS 88-MDS, the 5.25-inch minidisk controller | `cpm22/` has a two-disk CP/M 2.2 set, its BIOS and boot source, and a machine file. The minidisk acceptance tests boot this set. |
| `tarbell-sd/` | Tarbell single-density floppy controller | `cpm22/` has a CP/M 2.2 disk set and its BIOS source. The built-in `tarbell` machine boots the set. |

## Which images are in git

Only a small number of images are tracked. `.gitignore` names each tracked image one at a
time. The larger images are downloaded:

```sh
tools/fetch-disk-images.sh
```

The script downloads each missing image and checks it. The `README.md` in a disk set says
where the image came from and which machine file boots it.

## Where other disks are

| Location | What it holds |
|---|---|
| [`tests/media/`](../tests/media/) | The other disks that the acceptance tests boot. The media for a new test goes there. |
| [`examples/`](../examples/) | The examples that ship in the release package, each with its machine file and its media. |

## Read more

- [Disks](../docs/manual/disks.md) in the *User Manual*, for `MOUNT` and the disk formats.
- [`docs/sources.md`](../docs/sources.md) for where each image and each hardware fact came from.
