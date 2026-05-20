# SimpleFS

## Сборка

```bash
make clean
make
```

## Запуск

```bash
truncate -s 2M simplefs.img
LOOP=$(sudo losetup --find --show simplefs.img)
echo $LOOP
```

```bash
sudo insmod simplefs.ko disk_name=$LOOP sb_first_sector=0 sb_second_sector=128 max_filename_len=16 max_file_sectors=1
sudo mkdir -p /mnt/simplefs
sudo mount -t simplefs $LOOP /mnt/simplefs
```

## Проверка VFS

```bash
cat /proc/filesystems | grep simplefs
mount | grep simplefs
ls -v /mnt/simplefs | head
find /mnt/simplefs -maxdepth 1 -type f | wc -l
```

Для образа `2M` ожидается `4094` файла.

## Проверка superblock

Первая копия:

```bash
sudo dd if=$LOOP bs=512 skip=0 count=1 | hexdump -C | head
```

Вторая копия:

```bash
sudo dd if=$LOOP bs=512 skip=128 count=1 | hexdump -C | head
```

В обеих копиях должен быть magic `SFS`:

```text
00 53 46 53
```

Проверка восстановления из второй копии:

```bash
sudo umount /mnt/simplefs
sudo dd if=/dev/zero of=$LOOP bs=512 seek=0 count=1 conv=notrunc
sudo mount -t simplefs $LOOP /mnt/simplefs
sudo dd if=$LOOP bs=512 skip=0 count=1 | hexdump -C | head
```

После повторного mount первая копия должна снова содержать `SFS`.

## Проверка чтения и записи

```bash
./simplefs_test /mnt/simplefs
```

Программа обходит файлы, пишет случайное число, читает его обратно и сравнивает.

## Проверка IOCTL

Получить хэши файлов:

```bash
./simplefs_test /mnt/simplefs hashes
```

Получить mapping файла:

```bash
./simplefs_test /mnt/simplefs mapping f1
```

Обнулить все файлы:

```bash
./simplefs_test /mnt/simplefs write
./simplefs_test /mnt/simplefs zero
./simplefs_test /mnt/simplefs read
```

После `zero` ожидается:

```text
f1: read bytes: 00 00 00 00 00 00 00 00 empty
```

Стереть FS:

```bash
./simplefs_test /mnt/simplefs erase
sudo dd if=$LOOP bs=512 skip=0 count=1 | hexdump -C | head
sudo dd if=$LOOP bs=512 skip=128 count=1 | hexdump -C | head
```

После `erase` оба superblock-сектора должны быть нулевыми.
