// { dg-do run { target c++11 } }

// Copyright (C) 2008-2022 Free Software Foundation, Inc.
//
// This file is part of the GNU ISO C++ Library.  This library is free
// software; you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the
// Free Software Foundation; either version 3, or (at your option)
// any later version.

// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License along
// with this library; see the file COPYING3.  If not see
// <http://www.gnu.org/licenses/>.

// 23.2.3.n forward_list xxx [lib.forward_list.xxx]

#include <forward_list>
#include <testsuite_hooks.h>

// This test verifies the following:
//   
void
test01()
{
  std::forward_list<double> a = {0.0, 1.0, 2.0, 3.0, 4.0};
  std::forward_list<double> b = {1.0, 2.0, 3.0, 4.0, 4.0, 5.0};

  a.merge(std::move(b));

  std::forward_list<double> r = {0.0, 1.0, 1.0, 2.0, 2.0, 3.0, 3.0,
                                 4.0, 4.0, 4.0, 5.0};

  VERIFY(a == r);
}

int
main()
{
  test01();
  return 0;
}
