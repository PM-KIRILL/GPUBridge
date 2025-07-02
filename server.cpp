#include <arpa/inet.h>
#include <cstdlib>
#include <cstring>
#include <cuda_runtime_api.h>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <nvml.h>
#include <pthread.h>
#include <stdio.h>
#include <string>
#include <sys/socket.h>
#include <sys/uio.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>

#include <dlfcn.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <vector>

#include <list>
#include <map>

#include <csignal>
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>

#include "codegen/gen_server.h"
#include "rpc.h"

#define DEFAULT_PORT 14833
#define MAX_CLIENTS 10

struct ManagedPtr {
  void *src;
  void *dst;
  size_t size;
  cudaMemcpyKind kind;
  void *graph;

  ManagedPtr()
      : src(nullptr), dst(nullptr), size(0), kind(cudaMemcpyHostToDevice),
        graph(graph) {}

  ManagedPtr(void *src, void *dst, size_t s, cudaMemcpyKind k, void *graph)
      : src(src), dst(dst), size(s), kind(k), graph(graph) {}
};

std::map<conn_t *, std::list<ManagedPtr>> managed_ptrs;

static jmp_buf catch_segfault;
static void *faulting_address = nullptr;

int rpc_write(const void *conn, const void *data, const size_t size) {
  ((conn_t *)conn)->write_iov[((conn_t *)conn)->write_iov_count++] =
      (struct iovec){(void *)data, size};
  return 0;
}

static void segfault(int sig, siginfo_t *info, void *unused) {
  void *faulting_address = info->si_addr;
  int found = -1;
  size_t size = 0;

  printf("Обнаружено нарушение сегментации %p\n", faulting_address);

  for (auto &conn_entry : managed_ptrs) {
    for (auto &mem_entry : conn_entry.second) {
      void *allocated_ptr;
      size_t allocated_size = mem_entry.size;

      if ((uintptr_t)mem_entry.src <= (uintptr_t)faulting_address &&
          (uintptr_t)faulting_address <
              (uintptr_t)mem_entry.src + allocated_size) {
        allocated_ptr = mem_entry.src;
      } else if ((uintptr_t)mem_entry.dst <= (uintptr_t)faulting_address &&
                 (uintptr_t)faulting_address <
                     (uintptr_t)mem_entry.dst + allocated_size) {
        allocated_ptr = mem_entry.dst;
      } else {
        printf("Нет совпадения указателей, возврат.\n");
        continue;
      }

      found = 1;
      size = allocated_size;

      size_t page_size = sysconf(_SC_PAGE_SIZE);
      uintptr_t aligned_addr = (uintptr_t)faulting_address;

      void *allocated =
          mmap((void *)faulting_address, allocated_size, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

      if (allocated == MAP_FAILED) {
        perror("Ошибка выделения памяти по адресу нарушения");
        continue;
      }

      return;
    }
  }

  if (found == 1) {
    write(STDERR_FILENO, "НАЙДЕНО!!\n", 8);
    return;
  }

  struct sigaction sa;
  sa.sa_handler = SIG_DFL;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGSEGV, &sa, nullptr);
  raise(SIGSEGV);
}

typedef struct callBackData {
  conn_t *conn;
  void (*callback)(void *);
  void *data;
} callBackData_t;

void invoke_host_func(void *data) {
  callBackData_t *tmp = (callBackData_t *)(data);
  void *scuda_intercept_result;
  ManagedPtr copy;
  int found = 0;

  if (!tmp->conn) {
    std::cerr << "Ошибка: Соединение NULL в invoke_host_func" << std::endl;
    return;
  }

  printf("Вызов host-функции %p\n", tmp->callback);

  if (rpc_write_start_request(tmp->conn, 1) < 0) {
    std::cerr << "Ошибка: сбой rpc_write_start_request" << std::endl;
    return;
  }

  for (const auto &conn_entry : managed_ptrs) {
    for (const auto &mem_entry : conn_entry.second) {
      if (mem_entry.kind == cudaMemcpyDeviceToHost) {
        copy = mem_entry;
        found = 1;
      }
    }
  }

  rpc_write(tmp->conn, &found, sizeof(int));

  if (found > 0) {
    if (rpc_write(tmp->conn, &copy.dst, sizeof(void *)) < 0 ||
        rpc_write(tmp->conn, &copy.size, sizeof(size_t)) < 0)
      return;

    double *result = (double *)(copy.dst);

    if (rpc_write(tmp->conn, copy.dst, copy.size) < 0) {
      std::cerr << "Ошибка: сбой rpc_write при записи памяти" << std::endl;
      return;
    }
  }

  if (rpc_write(tmp->conn, &tmp->callback, sizeof(void *)) < 0) {
    std::cerr << "Ошибка: сбой rpc_write при записи callback" << std::endl;
    return;
  }

  if (rpc_wait_for_response(tmp->conn) < 0) {
    std::cerr << "Ошибка: сбой rpc_wait_for_response" << std::endl;
    return;
  }

  if (rpc_read(tmp->conn, &scuda_intercept_result, sizeof(void *)) < 0) {
    std::cerr << "Ошибка: сбой rpc_read при чтении scuda_intercept_result"
              << std::endl;
    return;
  }

  if (rpc_read_end(tmp->conn) < 0) {
    std::cerr << "Ошибка: сбой rpc_read_end" << std::endl;
    return;
  }
}

void maybe_destroy_graph_resources(void *graph) {
  for (auto &conn_entry : managed_ptrs) {
    auto &mem_list = conn_entry.second;

    for (auto it = mem_list.begin(); it != mem_list.end();) {
      if (it->graph == graph) {
        printf("Уничтожение mem_entry для графа\n");
        it = mem_list.erase(it);
      } else {
        ++it;
      }
    }
  }
}

void append_managed_ptr(const void *conn, void *srcPtr, void *dstPtr,
                        size_t size, cudaMemcpyKind kind, void *graph) {
  conn_t *connfd = (conn_t *)conn;

  if (!connfd) {
    std::cerr << "Ошибка: connfd равен null!" << std::endl;
    return;
  }

  if (managed_ptrs.find(connfd) == managed_ptrs.end()) {
    managed_ptrs[connfd] = std::list<ManagedPtr>();
  }

  managed_ptrs[connfd].push_back(ManagedPtr(srcPtr, dstPtr, size, kind, graph));
}

static void set_segfault_handlers() {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_flags = SA_SIGINFO;
  sa.sa_sigaction = segfault;

  if (sigaction(SIGSEGV, &sa, NULL) == -1) {
    perror("sigaction");
    exit(EXIT_FAILURE);
  }

  std::cout << "Обработчик нарушения сегментации установлен." << std::endl;
}

void client_handler(int connfd) {
  conn_t conn = {connfd, 1};
  conn.request_id = 1;
  if (pthread_mutex_init(&conn.read_mutex, NULL) < 0 ||
      pthread_mutex_init(&conn.write_mutex, NULL) < 0) {
    std::cerr << "Ошибка инициализации мьютекса." << std::endl;
    return;
  }

  printf("Клиент подключен.\n");

  while (1) {
    int op = rpc_dispatch(&conn, 0);

    auto opHandler = get_handler(op);
    if (opHandler(&conn) < 0) {
      std::cerr << "Ошибка обработки запроса." << std::endl;
    }
  }

  if (pthread_mutex_destroy(&conn.read_mutex) < 0 ||
      pthread_mutex_destroy(&conn.write_mutex) < 0)
    std::cerr << "Ошибка уничтожения мьютекса." << std::endl;

  close(connfd);
}

int main() {
  int port = DEFAULT_PORT;
  struct sockaddr_in servaddr, cli;
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd == -1) {
    printf("Ошибка создания сокета.\n");
    exit(EXIT_FAILURE);
  }

  set_segfault_handlers();

  char *p = getenv("SCUDA_PORT");

  if (p == NULL) {
    port = DEFAULT_PORT;
  } else {
    port = atoi(p);
  }

  memset(&servaddr, 0, sizeof(servaddr));
  servaddr.sin_family = AF_INET;
  servaddr.sin_addr.s_addr = INADDR_ANY;
  servaddr.sin_port = htons(port);

  const int enable = 1;
  if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0) {
    printf("Ошибка привязки сокета.\n");
    exit(EXIT_FAILURE);
  }

  if (bind(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) != 0) {
    printf("Ошибка привязки сокета.\n");
    exit(EXIT_FAILURE);
  }

  if (listen(sockfd, MAX_CLIENTS) != 0) {
    printf("Ошибка прослушивания.\n");
    exit(EXIT_FAILURE);
  }

  printf("Сервер ожидает подключения на порту %d...\n", port);

  while (1) {
    socklen_t len = sizeof(cli);
    int connfd = accept(sockfd, (struct sockaddr *)&cli, &len);

    if (connfd < 0) {
      std::cerr << "Ошибка принятия соединения сервером." << std::endl;
      continue;
    }

    std::thread client_thread(client_handler, connfd);
    client_thread.detach();
  }

  close(sockfd);
  return 0;
}