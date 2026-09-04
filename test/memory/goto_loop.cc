// The for loop's header has a single incoming edge, the back edge, because
// the body is only entered by the goto. Must be C++: the C frontend gives the
// header a second incoming edge.
int goto_into_loop(int *a, int n)
{
  int i = 0;
  goto body;
  for (; i < n; ++i) {
    switch (a[i]) {
    body:
      case 1: a[i] = 2; continue;
      case 2: a[i] = 1; break;
    }
    a[i]++;
  }
  return i;
}
