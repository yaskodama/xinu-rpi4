int main() {
  print(actor_send(0, "bump", 0));
  print(actor_send(0, "add", 40));
  return actor_send(0, "get", 0);
}
