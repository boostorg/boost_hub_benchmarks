/* Benchmark of boost::container::hub against plf::hive.
 * 
 * Copyright 2026 Joaquin M Lopez Munoz.
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 */

#include <algorithm>
#include <array>
#include <chrono>
#include <numeric>

std::chrono::high_resolution_clock::time_point measure_start, measure_pause;

template<typename F>
double measure(F f)
{
  using namespace std::chrono;

  static const int              num_trials = 10;
  static const milliseconds     min_time_per_trial(200);
  std::array<double,num_trials> trials;

  for(int i = 0; i < num_trials; ++i) {
    int                               runs = 0;
    high_resolution_clock::time_point t2;
    volatile decltype(f())            res; /* to avoid optimizing f() away */

    measure_start = high_resolution_clock::now();
    do{
      res = f();
      ++runs;
      t2 = high_resolution_clock::now();
    }while(t2 - measure_start<min_time_per_trial);
    trials[i] =
      duration_cast<duration<double>>(t2 - measure_start).count() / runs;
  }

  std::sort(trials.begin(), trials.end());
  return std::accumulate(
    trials.begin() + 2, trials.end() - 2, 0.0)/(trials.size() - 4);
}

void pause_timing()
{
  measure_pause = std::chrono::high_resolution_clock::now();
}

void resume_timing()
{
  measure_start += std::chrono::high_resolution_clock::now() - measure_pause;
}

#include <boost/container/hub.hpp>
#include <boost/core/detail/splitmix64.hpp>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <plf_hive.h>
#include <stdexcept>
#include <string>
#include <sstream>
#include <vector>

struct element
{
  element(int n_): n{n_} {}

#if defined(NONTRIVIAL_ELEMENT)
  ~element()
  {
    std::memset(payload, 0, sizeof(payload));
  }

  element(element&& x): n{x.n}
  {
    std::memcpy(payload, x.payload, sizeof(payload));
    std::memset(x.payload, 0, sizeof(payload));
  }

  element& operator=(element&& x)
  {
    n = x.n;
    std::memcpy(payload, x.payload, sizeof(payload));
    std::memset(x.payload, 0, sizeof(payload));
    return *this;
  }
#endif

  operator int() const { return n; }

  int n;
  char payload[ELEMENT_SIZE - sizeof(int)];
};

struct urbg
{
  using result_type = boost::uint64_t;

  static constexpr result_type min() { return 0; }
  static constexpr result_type max() { return (result_type)(-1); }

  urbg() = default;
  explicit urbg(result_type seed): rng{seed} {}

  result_type operator()() { return rng(); }

  boost::detail::splitmix64 rng;
};

template<typename Container, typename Iterator>
void erase_void(Container& x, Iterator it)
{
  x.erase(it);
}

template<typename... Args, typename Iterator>
void erase_void(boost::container::hub<Args...>& x, Iterator it)
{
  x.erase_void(it);
}

template<typename Container>
Container make(std::size_t n, double erasure_rate)
{
  std::uint64_t erasure_cut = 
    (std::uint64_t)(erasure_rate * (double)(std::uint64_t)(-1));

  Container                                 c;
  urbg                                      rng;
  std::vector<typename Container::iterator> iterators;

  iterators.reserve(n);
  for(std::size_t i = 0; i < n; ++i) iterators.push_back(c.insert((int)rng()));
  std::shuffle(iterators.begin(), iterators.end(), rng);
  for(auto it: iterators) {
    if(rng() < erasure_cut) erase_void(c, it);
  }
  return c;
}

template<typename Container>
void fill(Container& c, std::size_t n)
{
  urbg rng;
  if(n > c.size()) {
    n -= c.size();
    while(n--) c.insert((int)rng());
  }
}

static std::size_t min_size_exp = 3,
                   max_size_exp = 7;
static double      min_erasure_rate = 0.0,
                   max_erasure_rate = 0.9,
                   erase_rate_inc = 0.1;          

struct benchmark_result
{
  std::string                           title;
  std::vector<std::vector<std::string>> data;
};

template<typename FHive, typename FHub>
benchmark_result benchmark(const char* title, FHive fhive, FHub fhub)
{
  static constexpr std::size_t size_limit =
    sizeof(std::size_t) == 4?  800ull * 1024ull * 1024ull:
                              2048ull * 1024ull * 1024ull;

  benchmark_result res = {title};

  std::cout << std::string(41, '-') << "\n"
            << title << "\n"
            << "sizeof(element): " << sizeof(element) << "\n";
  std::cout << std::left << std::setw(11) << "" << "container size\n" << std::right
            << std::left << std::setw(11) << "erase rate" << std::right;
  for(std::size_t i = min_size_exp; i <= max_size_exp; ++i)
  {
    std::cout << "1.E" << i << " ";
  }
  std::cout << std::endl;
  for(double erasure_rate = min_erasure_rate; 
      erasure_rate <= max_erasure_rate; 
      erasure_rate += erase_rate_inc) {
    std::cout << std::left << std::setw(11) << erasure_rate << std::right << std::flush;

    res.data.push_back({});

    for(std::size_t i = min_size_exp; i <= max_size_exp; ++i) {
      std::ostringstream out;
      std::size_t        n = (std::size_t)std::pow(10.0, (double)i);
      if(n * sizeof(element) > size_limit) {
        out << "----";
        continue;
      }
      else{
        auto thive = measure([&] { return fhive(n, erasure_rate); });
        auto thub = measure([&] { return fhub(n, erasure_rate); });
        out << std::fixed << std::setprecision(2) << thive / thub;
      }
      std::cout << out.str() << " " << std::flush;
      res.data.back().push_back(out.str());
    }
    std::cout << std::endl;
  }
  return res;
}

template<typename Container>
struct create
{
  unsigned int operator()(std::size_t n, double erasure_rate) const
  {
    unsigned int res = 0;
    {
      auto c = make<Container>(n, erasure_rate);
      fill(c, n);
      res = (unsigned int)c.size();
      pause_timing();
    }
    resume_timing();
    return res;
  }
};

template<typename Container>
struct create_and_destroy
{
  unsigned int operator()(std::size_t n, double erasure_rate) const
  {
    auto c = make<Container>(n, erasure_rate);
    fill(c, n);
    return (unsigned int)c.size();
  }
};

template<typename Container>
struct prepare
{
  const Container& get_container(std::size_t n_, double erasure_rate_)
  {
    if(n_ != n || erasure_rate_ != erasure_rate) {
      pause_timing();
      n = n_;
      erasure_rate = erasure_rate_;
      c.clear();
      c.shrink_to_fit();
      c = make<Container>(n, erasure_rate);
      resume_timing();
    }
    return c;
  }

  std::size_t n = 0;
  double      erasure_rate = 0.0;
  Container   c;
};

template<typename Container>
struct for_each: prepare<Container>
{
  unsigned int operator()(std::size_t n, double erasure_rate)
  {
    unsigned int res = 0;
    auto& c = this->get_container(n, erasure_rate);
    for(const auto& x: c) res += (unsigned int)x; 
    return res;
  }
};

template<typename Container>
struct visit_all: prepare<Container>
{
  unsigned int operator()(std::size_t n, double erasure_rate)
  {
    unsigned int res = 0;
    auto& c = this->get_container(n, erasure_rate);
    c.visit_all([&] (const auto& x) { res += (unsigned int)x; });
    return res;
  }
};

template<typename Container>
struct sort
{
  unsigned int operator()(std::size_t n, double erasure_rate) const
  {
    pause_timing();
    auto c = make<Container>(n, erasure_rate);
    resume_timing();
    c.sort();
    return (unsigned int)c.size();
  }
};

using table = std::vector<benchmark_result>;

void write_table(const table& t, const char* filename)
{
  static std::size_t first_column_width = 15;
  static std::size_t data_column_width = (max_size_exp + 1 - min_size_exp) * 5;
  std::size_t        num_data_columns = t.size();
  std::size_t        table_width = first_column_width + 2 + num_data_columns * (data_column_width + 2) + 1;

  std::ofstream out(filename);

  auto data_horizontal_line =
    std::string(first_column_width + 2, ' ') + std::string(table_width - first_column_width - 2, '-');
  auto table_horizontal_line = std::string(table_width, '-');

  out << std::left;

  out << data_horizontal_line << "\n";

  out << "  " << std::setw(first_column_width) << " ";
  out << std::setw(table_width - first_column_width - 3) 
      << std::string("| sizeof(element): ") + std::to_string(ELEMENT_SIZE) << "|\n";

  out << data_horizontal_line << "\n";

  out << "  " << std::setw(first_column_width) << " ";
  for(const benchmark_result& res: t) {
    out << "| " << std::setw(data_column_width) << res.title;
  }
  out << "|\n";

  out << data_horizontal_line << "\n";

  out << "  " << std::setw(first_column_width) << " " ;
  for(int i = 0; i < num_data_columns; ++i) {
    out << "| " << std::setw(data_column_width) << "container size";
  }
  out << "|\n";

  out << table_horizontal_line << "\n";

  out << "| " << std::setw(first_column_width) << "erase rate";
  for(int i = 0; i < num_data_columns; ++i) {
    out << "| ";
    for(auto j = min_size_exp; j <= max_size_exp; ++j) {
          out << "1.E" << j << " ";
    }
  }
  out << "|\n";

  out << table_horizontal_line << "\n";
  
  std::size_t row = 0;
  for(double erasure_rate = min_erasure_rate; 
      erasure_rate <= max_erasure_rate; 
      erasure_rate += erase_rate_inc, ++row) {
    out << "| " << std::setw(first_column_width) << erasure_rate;
    for(const benchmark_result& res: t) {
      out << "| ";
      for(const auto& x: res.data[row]) {
        out << x << " ";
      }
    }
    out << "|\n";
  }

  out << table_horizontal_line << "\n";
}

int main(int argc,char* argv[])
{

  if(argc < 2) {
    std::cerr << "missing filename\n";
    return EXIT_FAILURE;
  }
  const char* filename = argv[1];
  
  try{
    using hive = plf::hive<element>;
    using hub = boost::container::hub<element>;

    table t;

    t.push_back(benchmark(
      "insert, erase, insert", 
      create<hive>{}, create<hub>{}));
    t.push_back(benchmark(
      "ins, erase, ins, destroy", 
      create_and_destroy<hive>{}, create_and_destroy<hub>{}));
    t.push_back(benchmark(
      "for_each", 
      for_each<hive>{}, for_each<hub>{}));
    t.push_back(benchmark(
      "visit_all", 
      for_each<hive>{}, visit_all<hub>{}));
    t.push_back(benchmark(
      "sort", 
      sort<hive>{}, sort<hub>{}));

    write_table(t, filename);
  }
  catch(const std::exception& e) {
    std::cerr << e.what() << std::endl;
  }
}
