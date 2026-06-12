int strlen(char *s) { int n; n = 0; while (*s) { n = n + 1; s = s + 1; } return n; }
int main() { char *m; m = "hello over HTTP"; puts(m); return strlen(m); }
