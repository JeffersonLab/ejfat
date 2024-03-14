
## Files in "ersap_engine" directory
These files are compiled into the shared libraries:

- **libersap_ejfat.so**,
- **libejfat_packetize_service.so**,
- **libejfat_assemble_service.so**,
- **libejfat_assemble_et_service.so**.

They are used to implement engines in ERSAP.
Vardan Gyurjyan can provide more details.
Creating these libs is done by specifying the -DBUILD_ERSAP=1 flag to cmake.

## Where to find dependencies

1) **et**  at  https://github.com/JeffersonLab/et
2) **hipo** at https://github.com/gavalian/hipo
