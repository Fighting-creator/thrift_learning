namespace cpp match_dao

struct User {
    1: i32 id,
    2: string name,
    3: i32 score
}

service Match {
    #add_user：在匹配池中添加几个玩家
    i32 add_user(1: User user, 2: string info),

    #remove_user：从匹配池中取消掉玩家
    i32 remove_user(1: User user, 2: string info),
}
