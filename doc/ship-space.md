# ship space documentation

our ship space coordinate system is z axis 'up' and right-handed:

```
          +Z
          |
          |
          |
          |
          *---------- +y
         /
        /
       /
      +x
```

within a ship:

* +y is fore
* +x is starboard
* +z is up


```
    +----------------\
    |                 \
    |      0---- +y    \
    |      |            \
    |      |             >
    |      |            /
    |      +x          /
    |                 /
    +----------------/
```


each unit along the axes is 1m

our space is dividied into 1m^3 blocks (voxels)


```
       1m
      <-->

      /--/      ^
     /  / |     | 1m
    +--+  |     -
    |  + /
    +--+/
```

