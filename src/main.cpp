#include <Arduino.h>

// put function declarations here:
int myFunction(int, int);

void setup()
{
  Serial.begin(9600);
  // put your setup code here, to run once:
  int result = myFunction(2, 3);
}

void loop()
{
  Serial.println("ping");
  delay(2000);
  Serial.println("pong");
  delay(2000);
}

// put function definitions here:
int myFunction(int x, int y)
{
  return x + y;
}