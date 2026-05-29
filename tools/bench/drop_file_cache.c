// drop_file_cache — evita o page-cache de UM arquivo sem root.
//
// Usa posix_fadvise(POSIX_FADV_DONTNEED), que pede ao kernel para descartar
// as páginas em cache do arquivo. Diferente de `echo 3 > drop_caches`, não
// precisa de privilégio e afeta só o arquivo alvo — ideal para medir o caso
// "cache frio" de forma reprodutível no benchmark do ingestor (H1).
//
// Uso: drop_file_cache <arquivo> [<arquivo> ...]
//
// Compilar: cc -O2 -o drop_file_cache drop_file_cache.c

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "uso: %s <arquivo> [<arquivo> ...]\n", argv[0]);
        return 2;
    }
    int rc = 0;
    for (int i = 1; i < argc; ++i) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            perror(argv[i]);
            rc = 1;
            continue;
        }
        // Garante que dados sujos (improvável em GFA read-only) não impeçam o
        // descarte, depois pede o DONTNEED sobre o arquivo inteiro (len=0).
        fdatasync(fd);
        if (posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED) != 0) {
            perror("posix_fadvise");
            rc = 1;
        }
        close(fd);
    }
    return rc;
}
