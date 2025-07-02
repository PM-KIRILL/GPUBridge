#include <sys/socket.h>

#include "rpc.h"
#include <iostream>
#include <string.h>
#include <unistd.h>


void *_rpc_read_id_dispatch(void *p) {
  conn_t *conn = (conn_t *)p;

  if (pthread_mutex_lock(&conn->read_mutex) < 0)
    return NULL;

  while (1) {
    while (conn->read_id != 0)
      pthread_cond_wait(&conn->read_cond, &conn->read_mutex);

    if (rpc_read(conn, &conn->read_id, sizeof(int)) < 0 || conn->read_id == 0 ||
        pthread_cond_broadcast(&conn->read_cond) < 0)
      break;
  }
  pthread_mutex_unlock(&conn->read_mutex);
  conn->rpc_thread = 0;
  return NULL;
}

int rpc_dispatch(conn_t *conn, int parity) {
  if (conn->rpc_thread == 0 &&
      pthread_create(&conn->rpc_thread, nullptr, _rpc_read_id_dispatch, (void *)conn) < 0) {
    return -1;
  }

  if (pthread_mutex_lock(&conn->read_mutex) < 0) {
    return -1;
  }

  int op;

  while (conn->read_id < 2 || conn->read_id % 2 != parity)
    pthread_cond_wait(&conn->read_cond, &conn->read_mutex);

  if (rpc_read(conn, &op, sizeof(int)) < 0) {
    pthread_mutex_unlock(&conn->read_mutex);
    return -1;
  }

  return op;
}

int rpc_read_start(conn_t *conn, int write_id) {
  if (pthread_mutex_lock(&conn->read_mutex) < 0)
    return -1;

  while (conn->read_id != write_id)
    if (pthread_cond_wait(&conn->read_cond, &conn->read_mutex) < 0)
      return -1;

  return 0;
}

int rpc_read(conn_t *conn, void *data, size_t size) {
  int bytes_read = recv(conn->connfd, data, size, MSG_WAITALL);
  if (bytes_read == -1) {
    printf("recv error: %s\n", strerror(errno));
  }
  return bytes_read;
}

int rpc_read_end(conn_t *conn) {
  int read_id = conn->read_id;
  conn->read_id = 0;
  if (pthread_cond_broadcast(&conn->read_cond) < 0 ||
      pthread_mutex_unlock(&conn->read_mutex) < 0)
    return -1;
  return read_id;
}

int rpc_wait_for_response(conn_t *conn) {
  int write_id = rpc_write_end(conn);
  if (write_id < 0 || rpc_read_start(conn, write_id) < 0)
    return -1;
  return 0;
}

int rpc_write_start_request(conn_t *conn, const int op) {
  if (pthread_mutex_lock(&conn->write_mutex) < 0) {
    return -1;
  }

  conn->write_iov_count = 2;
  conn->request_id = conn->request_id + 2;
  conn->write_id = conn->request_id;
  conn->write_op = op;
  return 0;
}

int rpc_write_start_response(conn_t *conn, const int read_id) {
  if (pthread_mutex_lock(&conn->write_mutex) < 0) {
    return -1;
  }

  conn->write_iov_count = 1;
  conn->write_id = read_id;
  conn->write_op = -1;
  return 0;
}

int rpc_write(conn_t *conn, const void *data, const size_t size) {
  conn->write_iov[conn->write_iov_count++] = (struct iovec){(void *)data, size};
  return 0;
}

int rpc_write_end(conn_t *conn) {
  conn->write_iov[0] = {&conn->write_id, sizeof(int)};
  if (conn->write_op != -1) {
    conn->write_iov[1] = {&conn->write_op, sizeof(unsigned int)};
  }

  if (writev(conn->connfd, conn->write_iov, conn->write_iov_count) < 0 ||
      pthread_mutex_unlock(&conn->write_mutex) < 0)
    return -1;
  return conn->write_id;
}