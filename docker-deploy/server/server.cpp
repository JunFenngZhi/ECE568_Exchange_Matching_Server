#include "server.h"

#define MAX_LENGTH 65536

std::mutex
    mtx_server;  // use in server for thread counting, dataBase connnect, resourceClean.
std::mutex mtx_queue;  // use in request queue

/*
  limit the max number of running thread. It should use the thread pool to handle threads
  to reduce the cost of create and destory new threads repeatly. There is still a bug. When running threads 
  reach the limit and the last request is received,push into the queue, requests in queue can not be handled
  in the future.
*/
void Server::run() {
  cout << "initialize server...." << endl;
  // initialize database
  connection * C = connectDB("exchange_server", "postgres", "passw0rd");
  initializeDB(C);
  disConnectDB(C);
  delete C;

  // create server socket, listen to port
  try {
    server_fd = createServerSocket(portNum);
  }
  catch (const std::exception & e) {
    std::cerr << e.what() << '\n';
    return;
  }

  // create IO thread to keep receiving requests
  cout << "create IO thread...." << endl;
  pthread_t t_IO;
  Thread_args * args_IO = new Thread_args(this, nullptr);
  pthread_create(&t_IO, NULL, _thread_run<Thread_args, Server, 2>, args_IO);

  // create a thread to handle new threads.
  cout << "create a thread to handle new threads....." << endl;
  pthread_t t_m;
  Thread_args * args_m = new Thread_args(this, nullptr);
  pthread_create(&t_m, NULL, _thread_run<Thread_args, Server, 3>, args_m);

  pthread_join(t_IO, NULL);
  pthread_join(t_m, NULL);
}

void Server::cleanResource(connection * C, ClientInfo * info) {
  mtx_server.lock();
  curRunThreadNum--;
  disConnectDB(C);
  delete info;
  delete C;
  cout << "thread#:" << curRunThreadNum << " exit" << endl;
  mtx_server.unlock();
}

/* ------------------------ "Request related functions" ------------------------ */
void Server::handleRequest(void * info) {
  ClientInfo * client_info = (ClientInfo *)info;

  // connect database in each thread
  connection * C = connectDB("exchange_server", "postgres", "passw0rd");

  // parse request
  unique_ptr<Request> r(nullptr);
  try {
    unique_ptr<XMLDocument> xml(convert_to_file(client_info->XMLrequest));
    int type = request_type(xml.get());
    if (type == CREATE) {
      unique_ptr<Request> temp_r(parse_create(xml.get()));
      r = std::move(temp_r);
    }
    else if (type == TRANSACTION) {
      unique_ptr<Request> temp_r(parse_trans(xml.get()));
      r = std::move(temp_r);
    }
  }
  catch (const std::exception & e) {
    std::cerr << e.what() << '\n';
    cleanResource(C, client_info);
  }

  // execute request
  r->executeRequest(C);

  // send back response
  try {
    sendResponse(client_info->client_fd, r->getResponseStr());
  }
  catch (const std::exception & e) {
    std::cerr << e.what() << '\n';
    cleanResource(C, client_info);
  }

  // exit successfully
  cleanResource(C, client_info);
}

void Server::recvRequest_socket(int client_fd, string & wholeRequest) {
  vector<char> buffer(MAX_LENGTH, 0);

  //receive first request.
  int len = recv(client_fd,
                 &(buffer.data()[0]),
                 MAX_LENGTH,
                 0);  // len 是recv实际的读取字节数
  if (len <= 0) {
    close(client_fd);
    throw MyException("fail to accept request.\n");
  }
  string firstRequest(buffer.data(), len);

  int contentLength = getContentLength(firstRequest);
  wholeRequest = firstRequest;
  int remainReceiveLen = contentLength - firstRequest.length();
  int receiveLen = 0;
  while (receiveLen < remainReceiveLen) {
    int len = recv(client_fd, &(buffer.data()[0]), MAX_LENGTH, 0);
    if (len < 0)
      break;
    string temp(buffer.data(), len);
    wholeRequest += temp;
    receiveLen += len;
  }
}

void Server::sendResponse(int client_fd, const string & XMLresponse) {
  int res = send(client_fd, &(XMLresponse.data()[0]), XMLresponse.length(), 0);
  if (res <= 0) {
    cout << "errno:" << errno << endl;
    throw MyException("fail to send back response.\n");
  }
}

/* ------------------------ "DB related functions" ------------------------ */
/*
  connect to specific database using the given userName and passWord, reutnr 
  the connection* C. It will throw an exception if fails. 
*/
connection * Server::connectDB(string dbName, string userName, string password) {
  mtx_server.lock();
  //connection * C = new connection("dbname=" + dbName + " user=" + userName + " password=" + password);   // use in real sys
  connection * C = new connection("host=db port=5432 dbname=" + dbName + " user=" + userName + " password=" + password);  // use in docker
  if (C->is_open()) {
    // cout << "Opened database successfully: " << C->dbname() << endl;
  }
  else {
    throw MyException("Can't open database.");
  }
  mtx_server.unlock();
  return C;
}

/*
  initialize database when server run at the first time. It will drop all the existing tables,
  and then create new ones.
*/
void Server::initializeDB(connection * C) {
  dropAllTable(C);
  createTable(C, "./sql/table.sql");
}

/*
  close the connection to database.
*/
void Server::disConnectDB(connection * C) {
  C->disconnect();
}

/* ------------------------ "thread create functions" ------------------------ */
/*
  keep checking the queue and curRunThreadNum. If possible, create new threads to handle
  request.
*/
void Server::threadManage() {
  // generate new thread for each request
  while (1) {
    while (curRunThreadNum < N_THREAD_LIMIT && !(requestQueue.empty())) {
      mtx_queue.lock();
      ClientInfo * info = requestQueue.front();
      requestQueue.pop();
      mtx_queue.unlock();

      Thread_args * args = new Thread_args(this, info);
      pthread_t thread;
      pthread_create(&thread, NULL, _thread_run<Thread_args, Server, 1>, args);
      mtx_server.lock();
      curRunThreadNum++;
      cout << "thread#:" << curRunThreadNum << " create" << endl;  //threads used to handleRequest, # begin from 1
      mtx_server.unlock();
    }
  }
}

/*
  keep receving new requests and put it into queue.
*/
void Server::recvRequest() {
  // IO
  int client_id = 1;
  while (1) {
    // server accept new connection
    int client_fd;
    string clientIp;
    try {
      client_fd = serverAcceptConnection(server_fd, clientIp);
    }
    catch (const std::exception & e) {
      std::cerr << e.what() << '\n';
      continue;
    }

    // server receive request
    string XMLrequest;
    recvRequest_socket(client_fd, XMLrequest);
    size_t firstLine = XMLrequest.find('\n', 0);
    XMLrequest = XMLrequest.substr(firstLine + 1);  // get rid of the number
    ClientInfo * info = new ClientInfo(client_fd, client_id, XMLrequest);
    mtx_queue.lock();
    requestQueue.push(info);
    mtx_queue.unlock();
    client_id++;
  }
}