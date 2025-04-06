#include <assert.h>

int main() {
  int x = 2;
  int y = x + 1;
  assert(y == 3); // This assertion should hold
  assert(y == 4); // This assertion should fail
  return 0;
}
