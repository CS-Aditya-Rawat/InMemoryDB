#include <iostream>

struct test {
  int x;
  int y;
  char c;
  bool b;
};

int main() {
  test s;
  s.x = 5; s.y=10; s.c='a';s.b=true;
  int *p = reinterpret_cast<int*>(&s);
  std::cout<<*p<<'\n';
  p++;
  std::cout<<*p<<'\n';
  p++;
  char *c = reinterpret_cast<char*>(p);
  std::cout<<*c<<'\n';
}

// Bottom Line:
// The result of reinterpret_cast cannot safely be used for anything other than being cast back to its original type.
// we should be very carefuly when using this cast..
// if we use this type of cast then it becomes non-portable product.
