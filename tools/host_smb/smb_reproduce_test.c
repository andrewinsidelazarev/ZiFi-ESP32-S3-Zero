#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void zifi_diagnostic_log_rpc_stage(const char* stage, int a, int b) {
  (void)stage; (void)a; (void)b;
}

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-raw.h>

int main(int argc, char **argv) {
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
    printf("FAIL: WSAStartup failed\n");
    return 1;
  }

  if (argc < 3) {
    printf("Usage: smb_reproduce_test host:port share\n");
    return 1;
  }
  const char *server = argv[1];
  const char *share = argv[2];

  struct smb2_context *smb2 = smb2_init_context();
  if (!smb2) {
    printf("FAIL: smb2_init_context failed\n");
    return 1;
  }
  smb2_set_security_mode(smb2, SMB2_NEGOTIATE_SIGNING_ENABLED);
  smb2_set_password(smb2, "zx");
  
  if (smb2_connect_share(smb2, server, share, "zx") != 0) {
    printf("FAIL: Connect to %s/%s failed: %s\n", server, share, smb2_get_error(smb2));
    smb2_destroy_context(smb2);
    return 1;
  }
  printf("SUCCESS: Connected to %s/%s\n", server, share);

  /* TEST 1: Sequential & Multiple Reads check */
  printf("\n--- TEST 1: Sequential & Chunked File Reading ---\n");
  struct smb2fh *fh_w = smb2_open(smb2, "read_test.bin", O_CREAT | O_WRONLY);
  if (fh_w) {
    uint8_t dummy[16384];
    for (int i = 0; i < 16384; ++i) dummy[i] = (uint8_t)(i & 0xFF);
    smb2_write(smb2, fh_w, dummy, 16384);
    smb2_close(smb2, fh_w);
  }
  struct smb2fh *fh_r = smb2_open(smb2, "read_test.bin", O_RDONLY);
  int read_ok = 0;
  if (fh_r) {
    uint8_t in_buf[16384] = {0};
    int r = smb2_read(smb2, fh_r, in_buf, 16384);
    smb2_close(smb2, fh_r);
    if (r == 16384) {
      read_ok = 1;
      printf("  Read 16384 bytes successfully!\n");
    } else {
      printf("  Read failed: got %d bytes\n", r);
    }
  }
  printf("RESULT TEST 1: %s\n", read_ok ? "PASS" : "FAIL");

  /* TEST 2: ReplaceIfExists Rename (Notepad / Explorer overwrite pattern) */
  printf("\n--- TEST 2: Rename ReplaceIfExists (Notepad Pattern) ---\n");
  struct smb2fh *f1 = smb2_open(smb2, "np_target.txt", O_CREAT | O_WRONLY);
  if (f1) {
    smb2_write(smb2, f1, (const uint8_t*)"ORIGINAL_CONTENT", 16);
    smb2_close(smb2, f1);
  }
  struct smb2fh *f2 = smb2_open(smb2, "np_temp.tmp", O_CREAT | O_WRONLY);
  if (f2) {
    smb2_write(smb2, f2, (const uint8_t*)"NEW_REPLACED_VAL", 16);
    smb2_close(smb2, f2);
  }

  int ren_res = smb2_rename(smb2, "np_temp.tmp", "np_target.txt");
  printf("  smb2_rename(np_temp.tmp -> np_target.txt) result: %d (error: %s)\n", ren_res, smb2_get_error(smb2));
  if (ren_res != 0) {
    printf("RESULT TEST 2: BUG CONFIRMED (Rename over existing target failed: %s)\n", smb2_get_error(smb2));
  } else {
    struct smb2fh *f_chk = smb2_open(smb2, "np_target.txt", O_RDONLY);
    char buf[32] = {0};
    if (f_chk) {
      smb2_read(smb2, f_chk, (uint8_t*)buf, 16);
      smb2_close(smb2, f_chk);
      printf("  Target file content after rename: \"%s\"\n", buf);
      if (strcmp(buf, "NEW_REPLACED_VAL") == 0) {
        printf("RESULT TEST 2: PASS (File successfully replaced)\n");
      } else {
        printf("RESULT TEST 2: FAIL (Unexpected content: \"%s\")\n", buf);
      }
    }
  }

  /* TEST 3: Timestamps check */
  printf("\n--- TEST 3: File Timestamps Check ---\n");
  struct smb2_stat_64 st3;
  if (smb2_stat(smb2, "np_target.txt", &st3) == 0) {
    printf("  np_target.txt smb2_mtime: %ld\n", (long)st3.smb2_mtime);
    if (st3.smb2_mtime <= 0 || st3.smb2_mtime < 315532800) {
      printf("RESULT TEST 3: BUG CONFIRMED (Timestamp is 0 / year 1601: %ld)\n", (long)st3.smb2_mtime);
    } else {
      printf("RESULT TEST 3: PASS (Valid timestamp: %ld)\n", (long)st3.smb2_mtime);
    }
  } else {
    printf("RESULT TEST 3: stat failed\n");
  }

  /* TEST 4: 52KB file copy read check (64KB buffer) */
  printf("\n--- TEST 4: 52KB file reading via 64KB buffer ---\n");
  struct smb2fh *fh_52_w = smb2_open(smb2, "evogram.bin", O_CREAT | O_WRONLY);
  if (fh_52_w) {
    uint8_t *big = (uint8_t*)malloc(53248);
    for (int i = 0; i < 53248; ++i) big[i] = (uint8_t)(i & 0xFF);
    smb2_write(smb2, fh_52_w, big, 53248);
    smb2_close(smb2, fh_52_w);
    free(big);
  }
  struct smb2fh *fh_52_r = smb2_open(smb2, "evogram.bin", O_RDONLY);
  int total_read_52 = 0;
  if (fh_52_r) {
    uint8_t *buf_64k = (uint8_t*)malloc(65536);
    int iters = 0;
    while (iters++ < 10) {
      int r = smb2_read(smb2, fh_52_r, buf_64k, 65536);
      printf("  Iteration %d: smb2_read(65536) returned %d\n", iters, r);
      if (r <= 0) break;
      total_read_52 += r;
    }
    smb2_close(smb2, fh_52_r);
    free(buf_64k);
  }
  printf("  Total read: %d / 53248\n", total_read_52);
  printf("RESULT TEST 4: %s\n", (total_read_52 == 53248) ? "PASS" : "FAIL");

  smb2_disconnect_share(smb2);
  smb2_destroy_context(smb2);
  return 0;
}
