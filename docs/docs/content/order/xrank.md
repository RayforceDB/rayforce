# Bucket Rank `xrank`

Assigns each element to a bucket based on its rank, dividing the data into n equal-sized groups.

```clj
↪ (xrank [30 10 20 40 50 60] 3)
[1 0 0 1 2 2]
↪ (xrank [1 2 3 4] 2)
[0 0 1 1]
↪ (xrank [40 10 30 20] 4)
[3 0 2 1]
```

!!! info
    - First argument is the vector to bucket
    - Second argument is the number of buckets
    - Returns bucket indices from 0 to n-1
    - Elements are distributed evenly across buckets based on rank
