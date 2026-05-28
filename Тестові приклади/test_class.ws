@startuml
title Діаграма класів: Електронна бібліотека
skinparam classAttributeIconSize 0

class User {
  - id: Integer
  - name: String
  + login(): Boolean
}
class Book {
  - title: String
  - author: String
}
class Order {
  - orderId: Integer
  + confirmOrder(): void
}

User <|-- Student
User "1" -- "0..*" Order : створює >
Order "1" *-- "1..*" Book : містить >
@enduml