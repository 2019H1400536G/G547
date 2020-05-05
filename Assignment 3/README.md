# G547
Device Drivers

Mylarapu Ramsai  2019H1400536G

silpa sathyan    2019H1400072G


                                  --SEQUENCE OF STEPS FOR EXECUTING USB BLOCK DRIVER CODE FOR READING AND WRITING FILES IN USB USING SCSI COMMANDS--


step1.  After creating main.c and make file for compiling use command $ make all.

step2.  remove kernel  default drivers for USB using commands   $ sudo rmmod uas (deregisters the interface driver uas)
                                                                $ sudo rmmod usb_storage (deregisters the interface driver usb_storage)
step3.  insert the user module for USB using  $ sudo insmod main.ko

step4.  one can verify using command $ lsmod whether module is loaded or not. 

step5.  Again repeat step2 to ensure inbuild drivers are removed. 

step6.  To verify whether usb driver is successfully loaded or not.  We use command sudo fdisk -l.

step7.  Use $ Sudo mkdir  /media/pusb command to create pusb folder in media directory.

step8.  use mount command for accessing pendrive  $ sudo mount -t vfat /dev/myDriver1 /media/pusb

step9.  go inside root  $ sudo -i. inorder to avoid using "sudo"

step10. we will open pusb folder created in media by using $ command cd /media/pusb

step11. command to see content of pendrive  $ ls 

step12. command to create new text file and writing into it  $ echo HELLO>test.txt

step13. command to read content of file  $ cat test.txt  output is HELLO

step14. For unmounting first we will go back to root folder by using command $ cd../..

step15. Then with the help of $ umount  /media/pusb command we unmount it. 

step16. command to come outside of root  $ logout

ste17.  remove pendrive and insert it again. we will see with the exsiting files a text file is successfully created.

 






	


