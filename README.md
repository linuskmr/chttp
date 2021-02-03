# chttp

chttp is a simple webserver serving static files and showing an overview of directories.

It is not reliable, secure or anything else you might want from a webserver.

## Installation

```bash
cmake CMakeLists.txt
make
```
## Starting the server


chttp `<PORT>` `<DIRECTORY>`
```bash
./chttp 8080 static/
```
